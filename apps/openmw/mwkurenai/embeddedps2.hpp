#ifndef OPENMW_MWKURENAI_EMBEDDEDPS2_H
#define OPENMW_MWKURENAI_EMBEDDEDPS2_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace MWKurenai
{
    /// Red Ninja inside OpenMW: loads PCSX2's core (the pcsx2-kurenai library from the [Kurenai] ps2 folder) and
    /// runs it on its own thread with no window, connected to this process's bridge. The player starts one program.
    /// Only once per process: PCSX2 keeps global state.
    class EmbeddedPs2
    {
    public:
        /// Null (with the reason logged) when [Kurenai] ps2 folder is empty or the library cannot be started.
        static std::unique_ptr<EmbeddedPs2> start(const std::string& bridgeName);
        ~EmbeddedPs2(); ///< Stops the game and waits a little for it.

        bool running() const { return !mDone; }

    private:
        EmbeddedPs2() = default;

        void* mLibrary = nullptr;
        void (*mStop)() = nullptr;
        std::vector<std::string> mArgs;
        std::vector<char*> mArgv;
        std::string mRoot, mData;
        std::thread mThread;
        std::atomic<bool> mDone{ false };
        int mResult = 0;
    };
}

#endif
