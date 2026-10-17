#!/usr/bin/env bash
# Print or install build/test dependencies for the Linux Fcitx5 IME.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: deps.sh [--install] [--print]

Debian / Ubuntu (Noble and later):
  sudo apt install build-essential g++ cmake extra-cmake-modules pkg-config gettext \
    libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev fcitx5-modules-dev \
    fcitx5 fcitx5-modules fcitx5-frontend-gtk3 fcitx5-frontend-qt5 \
    librime-dev librime-plugin-lua librime-bin \
    libopencc-dev libopencc-data libyaml-cpp-dev libleveldb-dev libmarisa-dev libgoogle-glog-dev libboost-regex-dev git \
    xvfb xdotool x11-utils dbus-x11 python3-gi gir1.2-gtk-3.0 libgtk-3-dev libicu-dev libsqlite3-dev libpoppler-glib-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libportal-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-pipewire pipewire wireplumber xdg-desktop-portal \
    libgtk-layer-shell-dev libxcb1-dev libxcb-xfixes0-dev libwayland-dev libssl-dev libcurl4-openssl-dev \
    qtbase5-dev fonts-inter fonts-noto-cjk fonts-noto-color-emoji weston wtype sway xdg-desktop-portal-wlr xdg-desktop-portal-gtk grim slurp
    # Runtime recommend for Wayland Capsule Ctrl+C (also a .deb Recommends):
    #   wl-clipboard

Arch:
  sudo pacman -S --needed base-devel cmake extra-cmake-modules fcitx5 fcitx5-qt \
    librime librime-lua opencc pkgconf gettext gtk3 icu sqlite poppler-glib gstreamer gst-plugins-base gst-plugins-good gst-plugin-pipewire libportal wayland libxcb qt5-base \
    xorg-server-xvfb xdotool python-gobject weston wtype \
    gtk-layer-shell libxcb openssl curl sway

Fedora:
  sudo dnf install gcc-c++ cmake extra-cmake-modules pkgconf-pkg-config gettext \
    fcitx5-devel librime-devel librime-lua opencc-devel \
    gtk3-devel libicu-devel sqlite-devel poppler-glib-devel gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good pipewire-gstreamer libportal-devel wayland-devel libxcb-devel qt5-qtbase-devel xorg-x11-server-Xvfb xdotool \
    python3-gobject weston wtype gtk-layer-shell libxcb-devel openssl-devel libcurl-devel sway
EOF
}

if [[ "${1:-}" == "--install" ]]; then
    if command -v apt-get >/dev/null; then
        sudo apt-get install -y \
            build-essential g++ cmake extra-cmake-modules pkg-config gettext \
            libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev fcitx5-modules-dev \
            fcitx5 fcitx5-modules fcitx5-frontend-gtk3 fcitx5-frontend-qt5 \
            librime-dev librime-plugin-lua librime-bin \
            libopencc-dev libopencc-data libyaml-cpp-dev libleveldb-dev libmarisa-dev libgoogle-glog-dev libboost-regex-dev git \
            xvfb xdotool x11-utils dbus-x11 python3-gi gir1.2-gtk-3.0 libgtk-3-dev libicu-dev libsqlite3-dev libpoppler-glib-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libportal-dev \
            gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-pipewire pipewire wireplumber xdg-desktop-portal \
            libgtk-layer-shell-dev libxcb1-dev libxcb-xfixes0-dev libwayland-dev libssl-dev libcurl4-openssl-dev \
            qtbase5-dev fonts-inter fonts-noto-cjk fonts-noto-color-emoji weston wtype sway xdg-desktop-portal-wlr xdg-desktop-portal-gtk grim slurp
    else
        echo "Use --print and install the matching packages for this distro." >&2
        exit 1
    fi
else
    usage
fi
