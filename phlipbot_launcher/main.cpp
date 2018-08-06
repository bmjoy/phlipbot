#include <filesystem>
#include <iostream>

#include <hadesmem/config.hpp>
#include <hadesmem/debug_privilege.hpp>
#include <hadesmem/injector.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/process_helpers.hpp>

namespace fs = std::experimental::filesystem;

int main_inner()
{
  // check that the bot dll exists
  std::wstring const dll_path_str =
    L"C:\\Users\\phlip9\\dev\\phlipbot\\Debug\\phlipbot.dll";
  fs::path dll_path = fs::path(dll_path_str);

  if (!fs::exists(dll_path)) {
    throw std::runtime_error("Cannot find phlipbot.dll");
  }

  // need privileges to inject
  hadesmem::GetSeDebugPrivilege();

  // get the WoW process handle
  std::wstring const proc_name = L"WoW.exe";
  std::unique_ptr<hadesmem::Process> process;
  process =
    std::make_unique<hadesmem::Process>(hadesmem::GetProcessByName(proc_name));

  std::wcout << "WoW.exe process id = " << process->GetId() << std::endl;

  // inject bot dll into WoW process
  uint32_t flags = 0;
  flags |= hadesmem::InjectFlags::kPathResolution;
  flags |= hadesmem::InjectFlags::kAddToSearchOrder;
  HMODULE module = hadesmem::InjectDll(*process, dll_path_str, flags);

  std::wcout << "successfully injected bot dll at base address = "
             << hadesmem::detail::PtrToHexString(module) << std::endl;

  // call remote Load() function
  hadesmem::CallResult<DWORD_PTR> const unload_res =
    hadesmem::CallExport(*process, module, "Load");

  std::wcout << "Called bot dll's Load() function" << std::endl;
  std::wcout << "Return value = " << unload_res.GetReturnValue() << std::endl;
  std::wcout << "LastError = " << unload_res.GetLastError() << std::endl;

  // wait for next input to call Unload() and free the bot dll
  std::wstring input;
  std::wcout << "Press Enter to Unload bot: ";
  std::wcin >> input;

  // call remote Unload() function
  hadesmem::CallResult<DWORD_PTR> const load_res =
    hadesmem::CallExport(*process, module, "Unload");

  std::wcout << "Called bot dll's Unload() function" << std::endl;
  std::wcout << "Return value = " << load_res.GetReturnValue() << std::endl;
  std::wcout << "LastError = " << load_res.GetLastError() << std::endl;

  // free the bot dll from the remote process
  hadesmem::FreeDll(*process, module);

  std::wcout << "Free'd the bot dll." << std::endl;

  return 0;
}

int main()
{
  try {
    return main_inner();
  } catch (...) {
    std::cerr << boost::current_exception_diagnostic_information() << std::endl;
  }
}