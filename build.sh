gdbus-codegen --interface-prefix org.bluez --generate-c-code gdbus-generated --c-generate-object-manager interface.xml

gcc -I. dbus-test.c gdbus-generated.c `pkg-config --cflags --libs glib-2.0 dbus-glib-1 gio-unix-2.0 libpico-1 gtk+-3.0` -o dbus-test


