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
- Starting tinywl+ will get you a black screen so one might want to start using the `-s <application>` ie `./tinywl -s sakura` to have it start an application when it starts.
- Would be nice to have [fcft](https://codeberg.org/dnkl/fcft) to render fonts so we can have titles in our titlebar.
- GTK does not play well with server side decorations(SSD). However, we can sorta force it to behave with some hacks included in `gtk_fix.sh`.
- Not as many protocols supported as [dwl](https://github.com/djpohly/dwl), but tinywl+ comes in lighter with lines of code(LOS) than dwl :)

### Build instructions and requirements
The requirements and build instructions are much like they are for original tinywl. ie:
<br>Install these dependencies:
- wlroots
- wayland-protocols

And run `make`.
