#include "import_pipeline.h"

#include "cache_manager.h"
#include "clang_complete.h"
#include "config.h"
#include "diagnostics_engine.h"
#include "include_complete.h"
#include "lsp.h"
#include "message_handler.h"
#include "platform.h"
#include "project.h"
#include "query_utils.h"
#include "queue_manager.h"
#include "timer.h"

#include <doctest/doctest.h>
#include <loguru.hpp>

#include <chrono>
#include <thread>

namespace {

// Checks if |path| needs to be reparsed. This will modify cached state
// such that calling this function twice with the same path may return true
// the first time but will return false the second.
//
// |from|: The file which generated the parse request for this file.
bool FileNeedsParse(int64_t write_time,
                    VFS* vfs,
                    bool is_interactive,
                    IndexFile* opt_previous_index,
                    const std::string& path,
                    const std::vector<std::string>& args,
                    const std::optional<std::string>& from) {
  {
    std::lock_guard<std::mutex> lock(vfs->mutex);
    if (vfs->state[path].timestamp < write_time) {
      LOG_S(INFO) << "timestamp changed for " << path
                  << (from ? " (via " + *from + ")" : std::string());
      return true;
    }
  }

  // Command-line arguments changed.
  auto is_file = [](const std::string& arg) {
    return EndsWithAny(arg, {".h", ".c", ".cc", ".cpp", ".hpp", ".m", ".mm"});
  };
  if (opt_previous_index) {
    auto& prev_args = opt_previous_index->args;
    bool same = prev_args.size() == args.size();
    for (size_t i = 0; i < args.size() && same; ++i) {
      same = prev_args[i] == args[i] ||
             (is_file(prev_args[i]) && is_file(args[i]));
    }
    if (!same) {
      LOG_S(INFO) << "args changed for " << path << (from ? " (via " + *from + ")" : std::string());
      return true;
    }
  }

  return false;
};

bool Indexer_Parse(DiagnosticsEngine* diag_engine,
                   WorkingFiles* working_files,
                   Project* project,
                   VFS* vfs,
                   ClangIndexer* indexer) {
  auto* queue = QueueManager::instance();
  std::optional<Index_Request> opt_request = queue->index_request.TryPopFront();
  if (!opt_request)
    return false;
  auto& request = *opt_request;
  ICacheManager cache;

  // Dummy one to trigger refresh semantic highlight.
  if (request.path.empty()) {
    IndexUpdate dummy;
    dummy.refresh = true;
    queue->on_indexed.PushBack(
        Index_OnIndexed(std::move(dummy), PerformanceImportFile()), false);
    return false;
  }

  Project::Entry entry;
  {
    std::lock_guard<std::mutex> lock(project->mutex_);
    auto it = project->absolute_path_to_entry_index_.find(request.path);
    if (it != project->absolute_path_to_entry_index_.end())
      entry = project->entries[it->second];
    else {
      entry.filename = request.path;
      entry.args = request.args;
    }
  }
  std::string path_to_index = entry.filename;
  std::unique_ptr<IndexFile> prev;

  // Try to load the file from cache.
  std::optional<int64_t> write_time = LastWriteTime(path_to_index);
  if (!write_time)
    return true;
  // FIXME Don't drop
  if (!vfs->Mark(path_to_index, g_thread_id, 1))
    return true;

  int reparse; // request.is_interactive;
  prev = cache.RawCacheLoad(path_to_index);
  if (!prev)
    reparse = 2;
  else {
    reparse = vfs->Stamp(path_to_index, prev->last_write_time);
    if (FileNeedsParse(*write_time, vfs, request.is_interactive, &*prev,
                       path_to_index, entry.args, std::nullopt))
      reparse = 2;
    for (const auto& dep : prev->dependencies)
      if (auto write_time1 = LastWriteTime(dep.first().str())) {
        if (dep.second < *write_time1) {
          reparse = 2;
          std::lock_guard<std::mutex> lock(vfs->mutex);
          vfs->state[dep.first().str()].stage = 0;
        }
      } else
        reparse = 2;
  }

  if (reparse < 2) {
    PerformanceImportFile perf;
    auto dependencies = prev->dependencies;
    if (reparse) {
      IndexUpdate update = IndexUpdate::CreateDelta(nullptr, prev.get());
      queue->on_indexed.PushBack(Index_OnIndexed(std::move(update), perf),
        request.is_interactive);
    }
    for (const auto& dep : dependencies)
      if (vfs->Mark(dep.first().str(), 0, 2)) {
        prev = cache.RawCacheLoad(dep.first().str());
        IndexUpdate update = IndexUpdate::CreateDelta(nullptr, prev.get());
        queue->on_indexed.PushBack(Index_OnIndexed(std::move(update), perf),
          request.is_interactive);
      }

    std::lock_guard<std::mutex> lock(vfs->mutex);
    VFS::State& state = vfs->state[path_to_index];
    if (state.owner == g_thread_id)
      state.stage = 0;
    return true;
  }

  LOG_S(INFO) << "parse " << path_to_index;

  std::vector<Index_OnIdMapped> result;
  PerformanceImportFile perf;
  auto indexes = indexer->Index(vfs, path_to_index, entry.args, {}, &perf);

  if (indexes.empty()) {
    if (g_config->index.enabled && request.id.Valid()) {
      Out_Error out;
      out.id = request.id;
      out.error.code = lsErrorCodes::InternalError;
      out.error.message = "Failed to index " + path_to_index;
      QueueManager::WriteStdout(kMethodType_Unknown, out);
    }
    vfs->Reset(path_to_index);
    return true;
  }

  for (std::unique_ptr<IndexFile>& curr : indexes) {
    // Only emit diagnostics for non-interactive sessions, which makes it easier
    // to identify indexing problems. For interactive sessions, diagnostics are
    // handled by code completion.
    if (!request.is_interactive)
      diag_engine->Publish(working_files, curr->path, curr->diagnostics_);

    std::string path = curr->path;
    if (!(vfs->Stamp(path, curr->last_write_time) || path == path_to_index))
      continue;
    LOG_S(INFO) << "emit index for " << path;
    prev = cache.RawCacheLoad(path);

    // Write current index to disk if requested.
    LOG_S(INFO) << "store index for " << path;
    Timer time;
    cache.WriteToCache(*curr);
    perf.index_save_to_disk = time.ElapsedMicrosecondsAndReset();

    vfs->Reset(path_to_index);
    if (entry.id >= 0) {
      std::lock_guard<std::mutex> lock(project->mutex_);
      for (auto& dep : curr->dependencies)
        project->absolute_path_to_entry_index_[dep.first()] = entry.id;
    }

    // Build delta update.
    IndexUpdate update = IndexUpdate::CreateDelta(prev.get(), curr.get());
    perf.index_make_delta = time.ElapsedMicrosecondsAndReset();
    LOG_S(INFO) << "built index for " << path << " (is_delta=" << !!prev << ")";

    Index_OnIndexed reply(std::move(update), perf);
    queue->on_indexed.PushBack(std::move(reply), request.is_interactive);
  }

  return true;
}

// This function returns true if e2e timing should be displayed for the given
// MethodId.
bool ShouldDisplayMethodTiming(MethodType type) {
  return
    type != kMethodType_TextDocumentPublishDiagnostics &&
    type != kMethodType_CclsPublishInactiveRegions &&
    type != kMethodType_Unknown;
}

}  // namespace

void Indexer_Main(DiagnosticsEngine* diag_engine,
                  VFS* vfs,
                  Project* project,
                  WorkingFiles* working_files,
                  MultiQueueWaiter* waiter) {
  auto* queue = QueueManager::instance();
  // Build one index per-indexer, as building the index acquires a global lock.
  ClangIndexer indexer;

  while (true)
    if (!Indexer_Parse(diag_engine, working_files, project, vfs, &indexer))
      waiter->Wait(&queue->index_request);
}

void QueryDb_OnIndexed(QueueManager* queue,
                       QueryDatabase* db,
                       SemanticHighlightSymbolCache* semantic_cache,
                       WorkingFiles* working_files,
                       Index_OnIndexed* response) {

  if (response->update.refresh) {
    LOG_S(INFO) << "Loaded project. Refresh semantic highlight for all working file.";
    std::lock_guard<std::mutex> lock(working_files->files_mutex);
    for (auto& f : working_files->files) {
      std::string filename = LowerPathIfInsensitive(f->filename);
      if (db->name2file_id.find(filename) == db->name2file_id.end())
        continue;
      QueryFile* file = &db->files[db->name2file_id[filename]];
      EmitSemanticHighlighting(db, semantic_cache, f.get(), file);
    }
    return;
  }

  Timer time;
  db->ApplyIndexUpdate(&response->update);

  // Update indexed content, inactive lines, and semantic highlighting.
  if (response->update.files_def_update) {
    auto& update = *response->update.files_def_update;
    time.ResetAndPrint("apply index for " + update.value.path);
    WorkingFile* working_file =
        working_files->GetFileByFilename(update.value.path);
    if (working_file) {
      // Update indexed content.
      working_file->SetIndexContent(update.file_content);

      // Inactive lines.
      EmitInactiveLines(working_file, update.value.inactive_regions);

      // Semantic highlighting.
      int file_id =
          db->name2file_id[LowerPathIfInsensitive(working_file->filename)];
      QueryFile* file = &db->files[file_id];
      EmitSemanticHighlighting(db, semantic_cache, working_file, file);
    }
  }
}

void LaunchStdinLoop(std::unordered_map<MethodType, Timer>* request_times) {
  std::thread([request_times]() {
    SetThreadName("stdin");
    auto* queue = QueueManager::instance();
    while (true) {
      std::unique_ptr<InMessage> message;
      std::optional<std::string> err =
          MessageRegistry::instance()->ReadMessageFromStdin(&message);

      // Message parsing can fail if we don't recognize the method.
      if (err) {
        // The message may be partially deserialized.
        // Emit an error ResponseMessage if |id| is available.
        if (message) {
          lsRequestId id = message->GetRequestId();
          if (id.Valid()) {
            Out_Error out;
            out.id = id;
            out.error.code = lsErrorCodes::InvalidParams;
            out.error.message = std::move(*err);
            queue->WriteStdout(kMethodType_Unknown, out);
          }
        }
        continue;
      }

      // Cache |method_id| so we can access it after moving |message|.
      MethodType method_type = message->GetMethodType();
      (*request_times)[method_type] = Timer();

      queue->for_querydb.PushBack(std::move(message));

      // If the message was to exit then querydb will take care of the actual
      // exit. Stop reading from stdin since it might be detached.
      if (method_type == kMethodType_Exit)
        break;
    }
  }).detach();
}

void LaunchStdoutThread(std::unordered_map<MethodType, Timer>* request_times,
                        MultiQueueWaiter* waiter) {
  std::thread([=]() {
    SetThreadName("stdout");
    auto* queue = QueueManager::instance();

    while (true) {
      std::vector<Stdout_Request> messages = queue->for_stdout.DequeueAll();
      if (messages.empty()) {
        waiter->Wait(&queue->for_stdout);
        continue;
      }

      for (auto& message : messages) {
        if (ShouldDisplayMethodTiming(message.method)) {
          Timer time = (*request_times)[message.method];
          time.ResetAndPrint("[e2e] Running " + std::string(message.method));
        }

        fwrite(message.content.c_str(), message.content.size(), 1, stdout);
        fflush(stdout);
      }
    }
  }).detach();
}

void MainLoop(MultiQueueWaiter* querydb_waiter,
              MultiQueueWaiter* indexer_waiter) {
  Project project;
  SemanticHighlightSymbolCache semantic_cache;
  WorkingFiles working_files;
  VFS vfs;
  DiagnosticsEngine diag_engine;

  ClangCompleteManager clang_complete(
      &project, &working_files,
      [&](std::string path, std::vector<lsDiagnostic> diagnostics) {
        diag_engine.Publish(&working_files, path, diagnostics);
      },
      [](lsRequestId id) {
        if (id.Valid()) {
          Out_Error out;
          out.id = id;
          out.error.code = lsErrorCodes::InternalError;
          out.error.message =
              "Dropping completion request; a newer request "
              "has come in that will be serviced instead.";
          QueueManager::WriteStdout(kMethodType_Unknown, out);
        }
      });

  IncludeComplete include_complete(&project);
  auto global_code_complete_cache = std::make_unique<CodeCompleteCache>();
  auto non_global_code_complete_cache = std::make_unique<CodeCompleteCache>();
  auto signature_cache = std::make_unique<CodeCompleteCache>();
  QueryDatabase db;

  // Setup shared references.
  for (MessageHandler* handler : *MessageHandler::message_handlers) {
    handler->db = &db;
    handler->waiter = indexer_waiter;
    handler->project = &project;
    handler->diag_engine = &diag_engine;
    handler->vfs = &vfs;
    handler->semantic_cache = &semantic_cache;
    handler->working_files = &working_files;
    handler->clang_complete = &clang_complete;
    handler->include_complete = &include_complete;
    handler->global_code_complete_cache = global_code_complete_cache.get();
    handler->non_global_code_complete_cache =
        non_global_code_complete_cache.get();
    handler->signature_cache = signature_cache.get();
  }

  // Run query db main loop.
  auto* queue = QueueManager::instance();
  while (true) {
    std::vector<std::unique_ptr<InMessage>> messages =
        queue->for_querydb.DequeueAll();
    bool did_work = messages.size();
    for (auto& message : messages) {
      // TODO: Consider using std::unordered_map to lookup the handler
      for (MessageHandler* handler : *MessageHandler::message_handlers) {
        if (handler->GetMethodType() == message->GetMethodType()) {
          handler->Run(std::move(message));
          break;
        }
      }

      if (message)
        LOG_S(ERROR) << "No handler for " << message->GetMethodType();
    }

    for (int i = 80; i--;) {
      std::optional<Index_OnIndexed> response = queue->on_indexed.TryPopFront();
      if (!response)
        break;
      did_work = true;
      QueryDb_OnIndexed(queue, &db, &semantic_cache, &working_files, &*response);
    }

    // Cleanup and free any unused memory.
    FreeUnusedMemory();

    if (!did_work) {
      auto* queue = QueueManager::instance();
      querydb_waiter->Wait(&queue->on_indexed, &queue->for_querydb);
    }
  }
}
