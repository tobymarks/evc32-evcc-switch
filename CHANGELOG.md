# Changelog

All notable changes to this project will be documented in this file.

This project follows semantic versioning once the first stable release is published.

## 0.1.0 - Unreleased

- Initial CYD firmware for EVCC mode switching.
- REST integration for EVCC `/api/state` and loadpoint mode updates.
- Runtime setup portal for Wi-Fi, EVCC host, port, loadpoint ID and display label.
- GitHub Pages web installer scaffold using ESP Web Tools.
- Stream and filter the large EVCC `/api/state` response to avoid incomplete JSON reads on ESP32.
- Reduce display flicker by redrawing only changed TFT regions after the initial layout render.
- Refine CYD UI with a stricter grid, larger touch targets and TFT_eSPI FreeSans GFX fonts as an interim typography upgrade.
