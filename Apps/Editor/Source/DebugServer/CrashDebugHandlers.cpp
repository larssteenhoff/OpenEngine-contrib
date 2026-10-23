#include "DebugServer/CrashDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <string>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterCrashDebugHandlers(EditorDebugServer& server)
{
    // debug_crash — deliberately kill the editor through a chosen crash path so
    // automation can gate the crash-evidence pipeline (SEH filter / terminate
    // handler / signal handler → minidump in every config + synchronous stderr
    // backtrace). An editor death that leaves no evidence in DebugFast/Release
    // gets misattributed to whatever IPC call happened nearby, so the pipeline
    // is exercised on demand rather than trusted.
    // kind: "av" (default) = access violation on the main thread,
    //       "terminate"    = std::terminate,
    //       "abort"        = std::abort (signal path).
    // The process dies inside the handler, so no response is ever sent — the
    // caller sees the connection drop, then verifies the dump + stderr output.
    server.RegisterHandler("debug_crash", [](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string kind = ctx.params.value("kind", "av");
        if (kind == "av")
        {
            volatile int* ptr = nullptr;
            *ptr = 42; // intentional AV → UnhandledSEHFilter
        }
        else if (kind == "terminate")
        {
            std::terminate(); // → TerminateHandler
        }
        else if (kind == "abort")
        {
            std::abort(); // → SIGABRT → SignalHandler
        }
        return Editor::RefuseRequest("Unknown crash kind: " + kind + " (expected av|terminate|abort)"); });
}

} // namespace GameEngine
