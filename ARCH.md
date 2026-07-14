This is a very complex project. Do not make sweeping changes to the code unless you know the complete context.

**DLL Loading Issue (Resolved):** The `SCDLLName("Mindful Trader - Version 2.0 Devel")` line in `SCStudies.cpp` is crucial for the DLL to load properly in Sierra Chart. Removing this line will cause the DLL to fail loading.