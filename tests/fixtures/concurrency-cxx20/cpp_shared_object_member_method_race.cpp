// SPDX-License-Identifier: Apache-2.0
// The thread runs one method of the object, its creator calls another. Both reach the same
// bytes, but neither names them: the state is a field, and the object is a local.
//
// What names it is the hand-off. The spawn says which object the thread was given, and the call
// that reaches `touch` passes the very same one, so the two accesses can finally be compared.
#include <thread>

class Counter
{
  public:
    void run()
    {
        _value = _value + 1;
    }

    void touch()
    {
        _value = _value + 1;
    }

  private:
    int _value = 0;
};

int main()
{
    Counter counter;
    std::thread worker(&Counter::run, &counter);

    counter.touch();
    worker.join();
    return 0;
}
