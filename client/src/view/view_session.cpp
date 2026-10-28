// view_session.cpp — viewSession (moved verbatim from play_main.cpp).
#include "view/view_session.h"

#include "solve/joindata.h"
#include "solve/replicate.h"
#include "view/view.h"
#include "instance/class_registry.h"
#include "instance/instance.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace rbxclient {

// Headful viewer: build a world from a captured chan1 stream and render
// the frozen snapshot (snapshot + streaming apply + event/prop/bulk
// counts; see StreamStats). Transport-independent: any future frontend
// (e.g. Studio) drives it with captured bytes. Returns 0 on clean
// viewer exit, 2 + log on failure.
int viewSession(const std::vector<uint8_t>& chan1, const std::string& shot,
                const LogFn& log) {
  std::string err;
  std::vector<rbxclient::ReplMessage> msgs;
  if (!rbxclient::walkReplFrames(chan1.data(), chan1.size(), msgs, &err)) {
    log(std::string("view: frame walk failed: ") + err);
    return 2;
  }
  const uint8_t* join_body = nullptr;
  size_t join_len = 0;
  for (const auto& m : msgs) {
    if (m.id == 0x97) {
      join_body = m.body.data();
      join_len = m.body.size();
      break;
    }
  }
  if (!join_body) {
    log("view: no 0x97 snapshot captured (offline mode?)");
    return 2;
  }
  std::vector<uint8_t> inner;
  if (!rbxclient::parseJoinOuter(join_body, join_len, inner, &err)) {
    log(std::string("view: 0x97 outer failed: ") + err);
    return 2;
  }
  rbxclient::JoinSchema schema;
  if (!rbxclient::parseJoinSchema(inner.data(), inner.size(), schema, &err)) {
    log(std::string("view: schema failed: ") + err);
    return 2;
  }
  std::vector<rbxclient::JoinRecord> recs;
  int snapshot_batches = 0, shared_batches = 0, shared_strings = 0;
  int skipped_batches = 0;
  for (const auto& m : msgs) {
    if (m.body.size() < 2 || m.body[0] != 0x83 || m.body[1] != 0x16)
      continue;
    std::vector<uint8_t> batch;
    if (!rbxclient::parseDeltaBatch(m.body.data(), m.body.size(), batch,
                                    &err))
      continue;
    if (batch.empty())
      continue; // empty inner (heartbeat): nothing to fold in
    const uint8_t flags = m.body[2];
    if (flags == 1) {
      // Shared-string dictionary sync (sub_342550C entries).
      std::vector<rbxclient::SharedString> ss;
      if (!rbxclient::parseSharedStrings(batch.data(), batch.size(), ss,
                                         &err)) {
        ++skipped_batches;
        continue;
      }
      ++shared_batches;
      shared_strings += int(ss.size());
      continue;
    }
    if (flags == 5) {
      ++skipped_batches; // NetAsset batch (unobserved live; no grammar)
      continue;
    }
    std::vector<rbxclient::JoinRecord> one;
    if (!rbxclient::parseJoinRecords(batch.data(), batch.size(), schema, one,
                                     &err)) {
      ++skipped_batches;
      continue;
    }
    ++snapshot_batches;
    recs.insert(recs.end(), one.begin(), one.end());
  }
  log("view: schema classes=" + std::to_string(schema.classes.size()) +
          " records=" + std::to_string(recs.size()) + " snap_batches=" +
          std::to_string(snapshot_batches) + " shared_batches=" +
          std::to_string(shared_batches) + " shared_strings=" +
          std::to_string(shared_strings) + " skipped_batches=" +
          std::to_string(skipped_batches));
  rbx::register_builtin_classes(); // idempotent; engine env owns the rest
  rbxclient::ApplyReport rep =
      rbxclient::applyJoinSnapshot(schema, recs, &err);
  if (!rep.root) {
    log(std::string("view: apply failed: ") + err);
    return 2;
  }
  log("view: spawned=" + std::to_string(rep.stats.spawned) +
          " grafted=" + std::to_string(rep.stats.grafted_to_root));
  // Streaming pass (recv-complete): fold live spawns/entry records into
  // the snapshot world; count events, prop updates, bulks, deferred types,
  // and failed walks. Per-message isolation (length framing): a failed
  // walk counts and the session continues with the next message, never
  // fatal.
  rbxclient::StreamStats sst;
  int stream_msgs = 0, stream_failed = 0;
  std::map<int, int> stream_stops;
  std::map<int, int> stream_types;
  const rbxclient::PropTagTable prop_tags =
      rbxclient::propTagsForSchema(schema);
  for (const auto& m : msgs) {
    if (m.body.size() < 2 || m.body[0] != 0x83 || m.body[1] == 0x16 ||
        m.body[1] == 0x97)
      continue;
    ++stream_msgs;
    rbxclient::Walk83 w;
    if (!rbxclient::walk83ItemsEx(m.body.data(), m.body.size(), schema,
                                  &prop_tags, w, &err)) {
      ++stream_failed;
      continue;
    }
    if (!w.complete)
      ++stream_stops[w.stop_type];
    for (const auto& it : w.items) {
      ++stream_types[it.type];
      if (it.type == 0x02) {
        std::vector<rbxclient::JoinProp> props = it.spawn.props_a;
        props.insert(props.end(), it.spawn.props_b.begin(),
                     it.spawn.props_b.end());
        rbxclient::applyStreamRecord(rep, schema, it.spawn.id, it.spawn.cls,
                                     props, it.spawn.parent, sst);
      } else if (it.type == 0x07) {
        rbxclient::logStreamEvent(schema, it.event, sst);
      } else if (it.type == 0x03) {
        ++sst.prop_updates;
      } else if (it.type == 0x78) {
        ++sst.bulks;
      } else if (it.type == 0x0d) {
        for (const auto& rec : it.delta0d.entries.records) {
          rbxclient::applyStreamRecord(rep, schema, rec.id, rec.cls,
                                       rec.props, rec.parent, sst);
          ++sst.entries_applied;
        }
      }
    }
  }
  {
    std::string stops;
    for (const auto& kv : stream_stops)
      stops += " " + std::to_string(kv.first) + ":" +
               std::to_string(kv.second);
    std::string stypes;
    for (const auto& kv : stream_types)
      stypes += " " + std::to_string(kv.first) + ":" +
                std::to_string(kv.second);
    log("view: stream msgs=" + std::to_string(stream_msgs) +
            " failed=" + std::to_string(stream_failed) + " stops=" +
            (stops.empty() ? "-" : stops) + " types=" +
            (stypes.empty() ? "-" : stypes) + " spawned=" +
            std::to_string(sst.spawned) + " updated=" +
            std::to_string(sst.updated) + " grafted=" +
            std::to_string(sst.stream_grafted) + " events=" +
            std::to_string(sst.events_logged) + " prop3=" +
            std::to_string(sst.prop_updates) + " bulks=" +
            std::to_string(sst.bulks));
    std::string evs;
    for (const auto& kv : sst.events_by_name)
      evs += " " + kv.first + ":" + std::to_string(kv.second);
    if (!evs.empty())
      log("view: events=" + evs);
  }
  const std::vector<rbxclient::ViewBox> boxes =
      rbxclient::collectBoxes(rep.root.get());
  log("view: boxes=" + std::to_string(boxes.size()));
  {
    // Framing diagnostics (permanent): finite positions, spawn-anchored
    // boxes, and the orbit focus. A NaN focus (or zero finite boxes)
    // renders an empty frame — this line tells which.
    const rbxclient::CameraFocus focus = rbxclient::focusCamera(boxes);
    int spawns = 0;
    for (const auto& b : boxes) {
      if (b.name == "SpawnLocation")
        ++spawns;
    }
    char fbuf[160];
    snprintf(fbuf, sizeof fbuf, "view: focus=(%.1f,%.1f,%.1f) r=%.1f spawns=%d",
             focus.cx, focus.cy, focus.cz, focus.radius, spawns);
    log(fbuf);
  }
  if (rbxclient::runViewer(boxes, shot, &err) != 0) {
    log(std::string("view: ") + err);
    return 2;
  }
  return 0;
}

} // namespace rbxclient
