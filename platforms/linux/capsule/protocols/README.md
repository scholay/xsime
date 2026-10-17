# Pinned data-control protocols

The unmodified XML files retain their upstream copyright/license headers. Build generated C and client headers with wayland-scanner; do not edit generated output.

- ext-data-control-v1.xml: wayland-protocols 1.45, Ubuntu package 1.45-1~ubuntu0.24.04.2. Upstream: https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/1.45/staging/ext-data-control/ext-data-control-v1.xml
- wlr-data-control-unstable-v1.xml: upstream swaywm/wlr-protocols commit b010a03648b88d143236de193bddbfea0c08bc84 (official archived predecessor of the freedesktop repository).

SHA-256:

- wlr-data-control-unstable-v1.xml: 5ac39552975b28d0ea42f636a79b7fc8e2d1fcfa31cbe173f549bd4b044ae2de
- ext-data-control-v1.xml: 293deba5dfdb974fbcf8dcf891a30a51a36a8bba0047dc6bdf6a03194089b7fb

`wlr-virtual-pointer-unstable-v1.xml` is from the same pinned wlr-protocols revision and retains its MIT header. It is linked only by the uninstalled capture test fixture to drive a region selector inside an isolated Sway desktop. SHA-256: `3ff6d540be0bc5228195bf072bde42117ea17945a5c2061add5d3cf97d6bb524`.
