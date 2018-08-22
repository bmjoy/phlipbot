#include <Windows.h>
#include <threadpoolapiset.h>

#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <boost/optional.hpp>
#include <boost/program_options.hpp>

#include <hadesmem/config.hpp>
#include <hadesmem/debug_privilege.hpp>
#include <hadesmem/detail/smart_handle.hpp>
#include <hadesmem/injector.hpp>
#include <hadesmem/module.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/process_helpers.hpp>


// TODO(phlip9): cmd line interface?
// > phlipbot_launcher inject
// > phlipbot_launcher eject
// > phlipbot_launcher watch
//    events: (either just poll or use some async io library (libuv?
//    boost::asio? windows api?))
//      + WoW.exe process stops or dll errors => stop watching? / or restart and
//      inject?
//      + dll changes => eject old dll then inject new dll
//      + launcher gets CTRL+C => eject dll and shutdown
//
// TODO(phlip9): watch for changes in the dll and then reinject

namespace fs = std::experimental::filesystem;
namespace po = boost::program_options;

using HandlerT = std::function<int(po::variables_map const&)>;


// TODO(phlip9): move into hadesmem/detail/smart_handle.hpp

struct CallbackEnvironmentPolicy {
  using HandleT = PTP_CALLBACK_ENVIRON;

  static constexpr HandleT GetInvalid() noexcept { return nullptr; }

  static bool Cleanup(HandleT handle)
  {
    ::DestroyThreadpoolEnvironment(handle);
    return true;
  }
};

using SmartCallbackEnvironment =
  hadesmem::detail::SmartHandleImpl<CallbackEnvironmentPolicy>;


struct ThreadpoolPolicy {
  using HandleT = PTP_POOL;

  static constexpr HandleT GetInvalid() noexcept { return nullptr; }

  static bool Cleanup(HandleT handle)
  {
    ::CloseThreadpool(handle);
    return true;
  }
};

using SmartThreadpool = hadesmem::detail::SmartHandleImpl<ThreadpoolPolicy>;


struct CleanupGroupPolicy {
  using HandleT = PTP_CLEANUP_GROUP;

  static constexpr HandleT GetInvalid() noexcept { return nullptr; }

  static bool Cleanup(HandleT handle)
  {
    ::CloseThreadpoolCleanupGroup(handle);
    return true;
  }
};

using SmartCleanupGroup = hadesmem::detail::SmartHandleImpl<CleanupGroupPolicy>;


struct ThreadpoolWaitPolicy {
  using HandleT = PTP_WAIT;

  static constexpr HandleT GetInvalid() noexcept { return nullptr; }

  static bool Cleanup(HandleT handle)
  {
    // Unregister the wait first by setting the event to nullptr
    ::SetThreadpoolWait(handle, nullptr, nullptr);
    // Then close it
    ::CloseThreadpoolWait(handle);
    return true;
  }
};

using SmartThreadpoolWait =
  hadesmem::detail::SmartHandleImpl<ThreadpoolWaitPolicy>;


boost::optional<hadesmem::Process>
get_proc_from_options(po::variables_map const& vm)
{
  try {
    if (vm.count("pid")) {
      DWORD const pid = vm["pid"].as<int>();
      return boost::make_optional<hadesmem::Process>(hadesmem::Process{pid});
    } else {
      auto const& pname = vm["pname"].as<std::wstring>();
      return boost::make_optional<hadesmem::Process>(
        hadesmem::GetProcessByName(pname));
    }
  } catch (hadesmem::Error const&) {
    // TODO(phlip9): possible false positives? check ErrorString?
    // TODO(phlip9): don't consume errors?
    return boost::none;
  }
}

DWORD_PTR call_dll_load(hadesmem::Process const& proc, HMODULE const mod)
{
  // call remote Unload() function
  hadesmem::CallResult<DWORD_PTR> const load_res =
    hadesmem::CallExport(proc, mod, "Load");

  return load_res.GetReturnValue();
}

DWORD_PTR call_dll_unload(hadesmem::Process const& proc, HMODULE const mod)
{
  // call remote Unload() function
  hadesmem::CallResult<DWORD_PTR> const load_res =
    hadesmem::CallExport(proc, mod, "Unload");

  return load_res.GetReturnValue();
}

HMODULE inject_dll(hadesmem::Process const& proc, std::wstring const& dll_name)
{
  uint32_t flags = 0;
  flags |= hadesmem::InjectFlags::kPathResolution;
  flags |= hadesmem::InjectFlags::kAddToSearchOrder;
  return hadesmem::InjectDll(proc, dll_name, flags);
}

boost::optional<hadesmem::Module const>
get_injected_dll(hadesmem::Process const& proc, std::wstring const& dll_name)
{
  // TODO(phlip9): use more robust manual iteration through module list
  try {
    return hadesmem::Module{proc, dll_name};
  } catch (hadesmem::Error const&) {
    // TODO(phlip9): possible false positives? check ErrorString?
    // TODO(phlip9): don't consume errors?
    return boost::none;
  }
}

struct WatchContext {
  bool is_done = false;
  std::mutex is_done_mutex;
  std::condition_variable is_done_condvar;
};

VOID CALLBACK ProcessExitCallback(PTP_CALLBACK_INSTANCE Instance,
                                  PVOID Parameter,
                                  PTP_WAIT Wait,
                                  TP_WAIT_RESULT WaitResult)
{
  (void)Instance;
  //(void)Parameter;
  (void)Wait;
  (void)WaitResult;

  auto ctx = static_cast<WatchContext*>(Parameter);

  {
    std::lock_guard<std::mutex> lock{ctx->is_done_mutex};
    ctx->is_done = true;
    std::wcout << "Got process exit callback\n";
  }

  ctx->is_done_condvar.notify_all();
}

int handler_inject(po::variables_map const& vm)
{
  // need privileges to inject
  hadesmem::GetSeDebugPrivilege();

  // get the WoW process handle
  auto const o_proc = get_proc_from_options(vm);

  // TODO(phlip9): optionally start new process if no existing process
  //               see hadesmem::CreateAndInject
  if (!o_proc.has_value()) {
    std::wcerr << "Error: process not running.\n";
    return 1;
  }

  std::wcout << "Process id = " << o_proc->GetId() << "\n";

  auto const& dll_name = vm["dll"].as<std::wstring>();

  // do nothing if dll already injected
  if (get_injected_dll(*o_proc, dll_name).has_value()) {
    std::wcerr << "Error: " << dll_name
               << " already injected, please eject first.\n";
    return 1;
  }

  // inject bot dll into WoW process
  HMODULE const mod = inject_dll(*o_proc, dll_name);

  std::wcout << "Successfully injected bot dll at base address = "
             << hadesmem::detail::PtrToHexString(mod) << "\n";

  // call remote Load() function
  auto const res = call_dll_load(*o_proc, mod);

  std::wcout << "Called bot dll's Load() function\n";
  std::wcout << "Return value = " << res << "\n";

  return 0;
}

int handler_eject(po::variables_map const& vm)
{
  // need privileges to eject
  hadesmem::GetSeDebugPrivilege();

  // get the WoW process handle
  auto const o_proc = get_proc_from_options(vm);
  if (!o_proc.has_value()) {
    std::wcerr << "Error: process not running.\n";
    return 1;
  }

  std::wcout << "Process id = " << o_proc->GetId() << "\n";

  auto const& dll_name = vm["dll"].as<std::wstring>();

  // get the phlipbot.dll handle in the WoW process
  auto const o_mod = get_injected_dll(*o_proc, dll_name);

  // do nothing if no dll is injected
  if (!o_mod.has_value()) {
    std::wcerr << "Error: no dll to eject\n";
    return 1;
  }

  auto const res = call_dll_unload(*o_proc, o_mod->GetHandle());

  std::wcout << "Called bot dll's Unload() function\n";
  std::wcout << "Return value = " << res << "\n";

  // free the bot dll from the remote process
  hadesmem::FreeDll(*o_proc, o_mod->GetHandle());
  std::wcout << "Free'd the bot dll.\n";

  return 0;
}

int handler_watch(po::variables_map const& vm)
{
  // need privileges to inject/eject
  hadesmem::GetSeDebugPrivilege();

  // get the WoW process handle
  auto const o_proc = get_proc_from_options(vm);

  // TODO(phlip9): optionally start new process if no existing process
  //               see hadesmem::CreateAndInject
  if (!o_proc.has_value()) {
    std::wcerr << "Error: process not running.\n";
    return 1;
  }

  std::wcout << "Process id = " << o_proc->GetId() << "\n";

  // auto const& dll_name = vm["dll"].as<std::wstring>();

  // Create a new threadpool with 1 thread for listening to events

  TP_CALLBACK_ENVIRON _cb_env;
  ::InitializeThreadpoolEnvironment(&_cb_env);
  auto const cb_env = SmartCallbackEnvironment{&_cb_env};

  auto const pool = SmartThreadpool{::CreateThreadpool(nullptr)};

  if (!pool.IsValid()) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"Failed to create Threadpool"}
                        << hadesmem::ErrorCodeWinLast{::GetLastError()});
  }

  ::SetThreadpoolThreadMaximum(pool.GetHandle(), 1);

  if (!::SetThreadpoolThreadMinimum(pool.GetHandle(), 1)) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"Failed to set min thread "
                                                 "count"}
                        << hadesmem::ErrorCodeWinLast{::GetLastError()});
  }

  ::SetThreadpoolCallbackPool(cb_env.GetHandle(), pool.GetHandle());

  auto const group = SmartCleanupGroup{::CreateThreadpoolCleanupGroup()};

  if (!group.IsValid()) {
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"Failed to create cleanup "
                                                 "group"}
                        << hadesmem::ErrorCodeWinLast{::GetLastError()});
  }

  ::SetThreadpoolCallbackCleanupGroup(cb_env.GetHandle(), group.GetHandle(),
                                      nullptr);


  // Set callback on WoW.exe process exit

  WatchContext ctx;

  auto const proc_wait = SmartThreadpoolWait{
    ::CreateThreadpoolWait(ProcessExitCallback, &ctx, cb_env.GetHandle())};

  ::SetThreadpoolWait(proc_wait.GetHandle(), o_proc->GetHandle(), nullptr);


  std::wcout << "Waiting on process exit\n";

  // TODO*phlip9): double free? seems like this might close all member callbacks
  // Wait for all callbacks to complete
  //::CloseThreadpoolCleanupGroupMembers(
  //  group.GetHandle(), /* fCancelPendingCallbacks */ FALSE, nullptr);

  // wait until ctx.is_done flag is triggered
  {
    std::unique_lock<std::mutex> lock{ctx.is_done_mutex};
    ctx.is_done_condvar.wait(lock, [&ctx] { return ctx.is_done; });
  }

  // block until all active callbacks complete
  ::WaitForThreadpoolWaitCallbacks(proc_wait.GetHandle(), FALSE);

  /*
  // Unload a currently loaded dll
  {
    auto const o_mod = get_injected_dll(*o_proc, dll_name);

    if (o_mod.has_value()) {
      auto const res = call_dll_unload(*o_proc, o_mod->GetHandle());
      std::wcout << "Called bot dll's Unload() function\n";
      std::wcout << "Return value = " << res << "\n";

      hadesmem::FreeDll(*o_proc, o_mod->GetHandle());
      std::wcout << "Free'd the bot dll.\n";
    }
  }

  // inject a fresh dll
  auto const mod = inject_dll(*o_proc, dll_name);

  std::wcout << "Successfully injected bot dll at base address = "
             << hadesmem::detail::PtrToHexString(mod) << "\n";

  // call remote Load() function
  auto const res = call_dll_load(*o_proc, mod);

  std::wcout << "Called bot dll's Load() function\n";
  std::wcout << "Return value = " << res << "\n";

  // TODO(phlip9): register on-change callback
  // TODO(phlip9): register signal handlers

  // if existing WoW.exe process has dll injected, unload it first
  // then load fresh dll
  //   when about to inject, existing dll might not be injected
  // then wait for changes in the dll to reload
  // while waiting, if WoW.exe crashes (restart? or shutdown?)
  */

  return 0;
}

int main_inner(int argc, wchar_t** argv)
{
  std::map<std::wstring, HandlerT> const cmd_handlers{
    {L"inject", handler_inject},
    {L"eject", handler_eject},
    {L"watch", handler_watch}};

  po::options_description desc("phlipbot_launcher [inject|eject|watch]");
  auto add = desc.add_options();
  add("help", "print usage");
  add(
    "dll,d",
    po::wvalue<std::wstring>()->default_value(L"phlipbot.dll", "phlipbot.dll"),
    "filename or path to the dll");
  add("pid,p", po::value<int>(), "the target process pid");
  add("pname,n",
      po::wvalue<std::wstring>()->default_value(L"WoW.exe", "WoW.exe"),
      "the target process name");
  add("command", po::wvalue<std::wstring>(), "inject|eject|watch");

  po::positional_options_description pos_desc;
  pos_desc.add("command", 1);

  po::wcommand_line_parser parser(argc, argv);
  parser.options(desc);
  parser.positional(pos_desc);

  po::variables_map vm;
  po::store(parser.run(), vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  if (vm.count("command")) {
    auto const& command = vm["command"].as<std::wstring>();

    if (cmd_handlers.count(command) != 1) {
      std::wcerr << "Error: invalid command: \"" << command << "\"\n\n";
      std::cout << desc << "\n";
      return 1;
    }

    auto const& handler = cmd_handlers.at(command);

    return handler(vm);
  }

  std::wcerr << "Error: missing command\n\n";
  std::cout << desc << "\n";

  return 1;
}

int main()
{
  try {
    // Get the command line arguments as wide strings
    int argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    hadesmem::detail::SmartLocalFreeHandle smart_argv{argv};

    if (argv == nullptr) {
      std::wcerr << "Error: CommandLineToArgvW failed\n";
      return 1;
    }

    return main_inner(argc, argv);
  } catch (...) {
    std::cerr << boost::current_exception_diagnostic_information() << "\n";
    return 1;
  }
}