# User thread tests

These self-checking x86-64 programs cover LATX user-mode thread creation and
fault recovery. They use pthreads, general registers, and legacy SSE2 only;
AVX is not required.

Build and run them natively:

```sh
./run-threads-tests.sh build build
./run-threads-tests.sh run build
```

To test LATS user mode, build the user binary and run both programs directly:

```sh
./latxbuild/build.sh -c user
./build-user/qemu-x86_64 ./build/user-thread-create
./build-user/qemu-x86_64 ./build/user-thread-fault-exit
```
