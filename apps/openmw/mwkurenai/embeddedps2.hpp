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
        /// Starts the game at the beginning of the engine's start-up, so it boots and reaches the stage while OpenMW
        /// loads; the next start() with the same bridge name hands this one over.
        static void startEarly();
        ~EmbeddedPs2(); ///< Stops the game and waits a little for it.

        bool running() const { return !mDone; }

        /// Whether Kurenai is driven right now: the player's own PCSX2 pad bindings are merged with OpenMW's only
        /// then. Cheap; only tells the library on a change.
        void setDriven(bool driven);

    private:
        EmbeddedPs2() = default;

        void* mLibrary = nullptr;
        void (*mStop)() = nullptr;
        void (*mSetDriven)(int) = nullptr; ///< Null with an older library.
        int mDriven = -1;
        std::vector<std::string> mArgs;
        std::vector<char*> mArgv;
        std::string mRoot, mData;
        std::thread mThread;
        std::atomic<bool> mDone{ false };
        int mResult = 0;
    };
}

#endif
