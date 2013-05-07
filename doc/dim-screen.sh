#!/bin/bash

# Example notifier script -- sets brightness to $min_brightness, then waits to
# be killed and restores previous brightness on exit.

min_brightness=0

get_brightness() {
    xbacklight -get

    # Or, for drivers without RandR backlight property support (e.g. radeon):
    #cat /sys/class/backlight/acpi_video0/brightness
}

set_brightness() {
    xbacklight -set $1

    # Or, for drivers without RandR backlight property support (e.g. radeon):
    #echo $1 | sudo tee /sys/class/backlight/acpi_video0/brightness
}

trap 'exit 0' TERM
trap "set_brightness $(get_brightness); kill %%" EXIT
set_brightness $min_brightness
sleep 2147483647 &
wait
