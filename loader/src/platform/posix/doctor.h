// `--doctor`. See doctor.cpp for what the report is for.
#pragma once

namespace xd3d {

// Print the environment report. `xbe_path` and `hdd_path` may be null; the report says so rather than
// assuming. Returns 0 when a usable Vulkan device was found and 1 when there was none, so a script can use
// it as a check without parsing the output.
int run_doctor(const char *xbe_path, const char *hdd_path);

} // namespace xd3d