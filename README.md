# Installation

Requirements:
 * Cmake
 * Cuda 
 * Psrdada
 
 Instructions:
 
```
$ mkdir build && cd build
$ cmake .. -DCMAKE_BUILD_TYPE=release
$ make
$ make install
```

## Linking to PSRDADA
You can link to a local installation of PSRDADA by setting the `LD_LIBRARY_PATH` and `PSRDADA_INCLUDE_DIR` enviroment variables.
