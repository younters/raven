#pragma once

namespace raven { class ApiRegistry; struct KernelContext; }

namespace raven_darwin {

// NtCreateFile, NtOpenFile, NtQueryAttributesFile, NtQueryFullAttributesFile, NtSetInformationFile and NtOpenSection, over the Win32 file handlers
// already in the registry; registered after them.
void register_nt_file_api(raven::ApiRegistry &registry, raven::KernelContext &context);

}
