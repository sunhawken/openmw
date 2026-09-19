#ifndef OPENMW_COMPONENTS_MISC_NIFBONEWRITER_H
#define OPENMW_COMPONENTS_MISC_NIFBONEWRITER_H

#include <string>
#include <string_view>

namespace VFS
{
    class Manager;
}

namespace Misc::NifBoneWriter
{
    // Adds deltaZ to the local translation of both standard breast bones in a
    // loose NIF. The selected VFS source must be writable; archive resources
    // are deliberately not extracted or modified.
    bool addBreastZ(const VFS::Manager& vfs, std::string_view meshFile, float deltaZ, std::string& error);
}

#endif
