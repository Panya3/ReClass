# llvm-demangle (vendored)

Microsoft's MSVC name demangler, vendored from the LLVM project at tag
[`llvmorg-20.1.0`](https://github.com/llvm/llvm-project/tree/llvmorg-20.1.0)
(LLVM 20.1.0).

## What is this for?

`src/rtti.cpp` calls `llvm::microsoftDemangle()` (via `demangleRttiName`) to
demangle MSVC RTTI type descriptor names (`.?AV<name>@@`). LLVM's demangler
is the reference implementation of MSVC name mangling — it handles template
instantiations (`?$...@...@@` with back-references), anonymous namespaces,
nested scope chains, etc. — which the project's small in-house parser cannot.
The in-house parser remains as a fallback when LLVM rejects the input.

Only the MSVC demangler was vendored (Itanium names still go through
`abi::__cxa_demangle` on GCC/Clang/MinGW builds).

## Included files

All unchanged from upstream, except the LICENSE.txt/README you're reading:

- `src/MicrosoftDemangle.cpp`
- `src/MicrosoftDemangleNodes.cpp`
- `include/llvm/Demangle/Demangle.h`
- `include/llvm/Demangle/DemangleConfig.h`
- `include/llvm/Demangle/ItaniumDemangle.h`
- `include/llvm/Demangle/ItaniumNodes.def`
- `include/llvm/Demangle/MicrosoftDemangle.h`
- `include/llvm/Demangle/MicrosoftDemangleNodes.h`
- `include/llvm/Demangle/StringViewExtras.h`
- `include/llvm/Demangle/Utility.h`

Upstream says of these sources: *"This file has no dependencies on the rest
of LLVM so that it can be easily reused in other programs such as
libcxxabi"* — hence a standalone vendored copy is safe.

## License

Apache License 2.0 WITH LLVM-exception — see `LICENSE.txt` (copied from
upstream `llvm/LICENSE.TXT`). Every source file carries its own SPDX header.
