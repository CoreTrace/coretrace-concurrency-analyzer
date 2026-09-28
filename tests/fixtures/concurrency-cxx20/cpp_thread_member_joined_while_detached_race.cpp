// SPDX-License-Identifier: Apache-2.0
// One thread detaches the std::thread the object holds while the owner joins it, each through a
// method of the object. Joining and detaching both write the handle, so the two calls race on
// the same member.
#include <thread>

struct Owner
{
    std::thread worker;

    void finish()
    {
        worker.join();
    }

    void release()
    {
        worker.detach();
    }
};

static Owner owner;

static void release_elsewhere()
{
    owner.release();
}

int main()
{
    owner.worker = std::thread([] {});
    std::thread other(release_elsewhere);
    owner.finish();
    other.join();
    return 0;
}
