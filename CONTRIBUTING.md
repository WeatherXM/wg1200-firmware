# Contributing to WG1200 Firmware

Thank you for your interest in contributing to the WeatherXM WG1200 gateway firmware! We welcome contributions from developers, IoT builders, and community members.

---

## Code of Conduct

Please be respectful, constructive, and collaborative in all communications, issues, and pull requests.

---

## How to Contribute

### Reporting Issues

* Check existing GitHub Issues before submitting a new bug report.
* When reporting issues, please include:
  * Firmware version and commit hash
  * Hardware target (e.g. WG1200 standard, Seeed XIAO ESP32-S3, LilyGO)
  * Serial monitor logs (`pio run -e wg1200_open -t monitor`)
  * Steps to reproduce the issue
* **Security Issues**: Please do **NOT** open public issues for security vulnerabilities. Follow our [Security Policy](SECURITY.md).

### Submitting Pull Requests

1. **Fork and Branch**:
   * Fork the repository and create a feature branch off `main`:
     ```bash
     git checkout -b feature/my-new-feature
     ```
2. **Build Verification**:
   * Verify your changes compile cleanly using PlatformIO:
     ```bash
     pio run -e wg1200_open
     ```
   * Never commit production secrets, private tokens, or hardware eFuse burns.
3. **PR Description**:
   * Clearly explain what changed, why it changed, and how it was tested.

---

## Developer Certificate of Origin (DCO) & Licensing

By contributing to this repository, you agree that your contributions are made under the **Apache License 2.0** terms, as specified in the [LICENSE](LICENSE) file.
