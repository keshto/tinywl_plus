# tinywl+
A simple stacking wayland compositor based on tinywl. Where features can be applied to [wlroot's tinywl](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/0.15/tinywl) example.

### Inspiration
- It was hard to find an easy way to add functionality to tinywl without having to understand very different code bases such as sway, etc. This was made so people can start developing and adding, what I think should be part of tinywl or at least in the examples, things to their compositor or contribute here for common use cases.
- There is not enough stacking window managers for wayland that are simple to start from.
- JWM ♥

### Goals
- Each commit should encapture an idea. Bug fixes etc should be amended to the relevant commit.
- Keep things simple and as easy and basic as possible.
- Maybe have patch sets that are optional or larger in seperate patches.

### Notes
- Would be nice to have [fcft](https://codeberg.org/dnkl/fcft) to render fonts so we can have titlebars
- It is recomended that you install alacritty with tinywl+ so that ounce in the compositor you can press just alt to open alacritty with a help message
- Not as many protocols supported as [dwl](https://github.com/djpohly/dwl), but tinywl+ comes in lighter with lines of code (LOS) than dwl :)

### Build instructions and requirements
The requirements and build instructions are much like they are for original tinywl except we use meson. ie:
<br>Install these dependencies:
- wlroots
- wayland-protocols

And run `meson build` followed by `ninja -C build` and then `ninja -C build install`.
