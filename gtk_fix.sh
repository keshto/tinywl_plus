#!/bin/bash

# This adds a custom css rules to remove the curved corners and drop shadows
echo '/** Some apps use titlebar class and some window */
.titlebar,
/** Remove curved windows */
window {
	border-radius: 0;
	box-shadow: none;
}

/** Remove shadows */
decoration {
	box-shadow: none;
	margin: 0;
}
decoration:backdrop {
	box-shadow: none;
}
' >> ~/.config/gtk-3.0/gtk.css

# In wayalnd the settings.ini file is not used so have to set with gsettings instead
# Remove the CSD close etc buttons
gsettings set org.gnome.desktop.wm.preferences button-layout menu:none
# Don't do anything window manager specific when clicking the headder bar
gsettings set org.gnome.desktop.wm.preferences action-double-click-titlebar none
gsettings set org.gnome.desktop.wm.preferences action-right-click-titlebar none
gsettings set org.gnome.desktop.wm.preferences action-middle-click-titlebar none
