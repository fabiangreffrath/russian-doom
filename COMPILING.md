### Building Russian DOOM on Windows

##### Step 1: Getting Code::Blocks and MinGW compiler

The primary IDE for building Russian DOOM is Code::Blocks, which may be downloaded on it's [official site](http://www.codeblocks.org/downloads/binaries). It is recommended to download a package that includes the compiler from TDM-GCC pre-installed. Otherwise, in case you want to install compiler manually, you can find the TDM-GCC installer [here](http://tdm-gcc.tdragon.net/).

##### Step 2: Installing the Development Libraries

Next you will need development libraries for [SDL2](https://www.libsdl.org/download-2.0.php), [SDL2 Mixer](https://www.libsdl.org/projects/SDL_mixer/) and [SDL2 Net](https://www.libsdl.org/projects/SDL_net/). Pay attention, you will need MinGW libraries, not Visual C++. After you get the development libraries, extract the files and copy the 'include', 'lib' and 'bin' folders to the top of your TDM-GCC directory (i.e. C:\TDM-GCC). Also make sure to move the files from the 'include\SDL' subfolder to the main 'include' folder. At this point you should be ready to build Russian DOOM.

##### Step 3: Compiling Russian DOOM

In the source directory of Russian DOOM, you will find the Code::Blocks files in the 'codeblocks' folder, the workspace file is called 'russian_doom.workspace'.

After you open the workspace, you can select what type of build you want to create: Debug or Release. In case you are going to debug Russian DOOM, you should compile a debug build (which is quite a lot bigger than the release), otherwise, you can just compile a release build. In order to start building, click on Build and then Rebuild workspace.

At the end of the compilation process, you will find your new Russian DOOM build in the 'bin' folder at the top of the source directory.


Please note: to run the compiled game executables you will need folder 'russian' (which contains translated wad files) placed in the folder with executables, as well as official IWAD files.