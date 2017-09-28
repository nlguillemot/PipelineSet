#How to use this example project

It should mostly "just work" to open the solution and run it.

To test the live-reloading, use it as follows:

1. Open the project in Visual Studio
2. Start the project without debugging (Debug->Start Without Debugging, or Ctrl+F5)
3. Open test.cs.hlsl and make a modification to the magic number "1337" (eg: change it to 1234)
4. Rebuild the project *while the program is still running* (Build->Build Solution, or Ctrl+Shift+B)
5. Observe that the command line is now outputting your new magic number instead of 1337.
