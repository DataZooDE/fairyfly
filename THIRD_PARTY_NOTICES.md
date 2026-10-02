# Third-party software

fairyfly is licensed under the [Business Source License 1.1](LICENSE). It is built from its own source code together with the
open-source libraries below (installed with vcpkg, linked statically). There are no proprietary third-party components;
the only non-open-source parts are Windows system libraries (SAP GUI Scripting is driven through the COM
interface of the SAP GUI installed on the user's machine, nothing of SAP is shipped).

| Library | Licence | Used for |
|---|---|---|
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause | command line parsing |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [spdlog](https://github.com/gabime/spdlog) | MIT | logging |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | MIT | YAML configuration |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG Reference Library License v2 | screenshots |
| [CImg](https://cimg.eu) | CeCILL-C v1 (free software, LGPL-style) | image cropping and scaling |
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 | unit tests only, not part of `fairyfly.exe` |

The exact licence texts are installed by vcpkg under `vcpkg_installed/<triplet>/share/<port>/copyright`.
