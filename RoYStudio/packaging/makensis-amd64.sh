#!/bin/sh
# Runs makensis with a 64-bit (amd64-unicode) installer stub. CPack passes its arguments through.
exec makensis "-XTarget amd64-unicode" "$@"
