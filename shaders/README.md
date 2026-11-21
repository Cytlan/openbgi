# Shader files

`anime4k/` holds shader files of [Anime4K](https://github.com/bloc97/Anime4K)
4.0.1, unchanged: the ones its six modes are made of, in the format of
mpv's user shaders. The Linux and Windows programs carry them and offer
the modes as filters in the display settings (F9) where the picture is
shown through OpenGL ([docs/manual/controls.md](../docs/manual/controls.md),
[docs/dev/os_layer.md](../docs/dev/os_layer.md#the-picture-through-opengl)).

The files are under the MIT License, Copyright (c) 2019-2021 bloc97;
`Anime4K_AutoDownscalePre_x2.glsl` and `Anime4K_AutoDownscalePre_x4.glsl`
are in the public domain (the Unlicense). Each file begins with its
terms. See [NOTICES](../NOTICES).

Every `*.glsl` file in a folder here is packed into the program
(`tools/mkpack.py`). Which files make up a filter is the table `kChains`
in `src/host/os_gl.c`.
