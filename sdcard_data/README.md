# SD Card Payload

This directory mirrors the expected filesystem layout for the root of your Micro SD card.
To use the features that rely on the SD card (e.g. dynamic backgrounds, large dynamic message files, etc.), you should copy the **contents** of this folder directly to the root of a FAT32 formatted Micro SD card.

## Structure
- `/backgrounds/offline/`: Place 466x466 `.jpg` or `.jpeg` files here. They will be dynamically loaded behind the Offline Mode welcome messages.
- `/messages/`: Place your `welcome.txt` or any other `.txt` offline AI message dumps here (if you choose to migrate them off of the internal SPIFFS storage).

> **Note**: Do not copy the folder `sdcard_data` itself to the SD card, only the folders *inside* it (`backgrounds`, `messages`, etc).
