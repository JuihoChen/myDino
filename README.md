# myDino

myDino is a Qt-based utility for SAS/SMP device discovery and control on Linux systems.  
It provides tools to scan, query, and manage SAS expanders and related hardware using both kernel and MPT interfaces.

## Features

- **SAS/SMP Discovery:**  
  Scan and enumerate SAS expanders and devices via `/dev/bsg` and `/dev/mpt*` interfaces.
- **Detailed Device Information:**  
  Retrieve and display SAS addresses, enclosure IDs, device slot numbers, link rates, and more.
- **Low-level SMP Commands:**  
  Send SMP requests and interpret responses, including error handling and debug output.
- **Qt Integration:**  
  Uses Qt for string handling, logging, and (optionally) GUI integration.
- **Debugging Support:**  
  Verbose output and debug messages for troubleshooting and development.

## Requirements

- Linux (with SAS/SMP hardware and drivers)
- Qt 5 (Widgets, Core)
- CMake (recommended) or qmake
- C++17 compatible compiler

## Building

### Using CMake

```bash
git clone https://github.com/yourusername/myDino.git
cd myDino
mkdir build
cd build
cmake ..
make
```

### Using Qt Creator

- Open the project folder in Qt Creator.
- Configure the kit and build as usual.

## Usage

Run the application with optional verbosity:

```bash
sudo ./myDino [-v]
```

- `-v` or `--verbose`: Enable verbose/debug output.

The tool will scan for SAS expanders and print detailed information about each discovered device and phy.

## Code Structure

- `main.cpp` — Application entry point, command-line parsing, and main window setup.
- `widget.h/cpp` — Main Qt Widget and UI logic.
- `smp_discover.h/cpp` — Core logic for SAS/SMP device discovery and control.
- `smp_lib.h/cpp` — SMP protocol helpers and utilities.
- `mpi3mr_app.h/cpp` — MPT/MPI3MR interface logic.
- `lsscsi.h/cpp` — SCSI device listing and related utilities.
- `mpi_type.h`, `mpi.h`, `mpi_sas.h`, etc. — Protocol and hardware definitions.
- `resources/` — (Optional) Images, icons, or other assets.

## Disclaimer

This tool is intended for advanced users and developers working with SAS/SMP hardware.  
Use with caution, as low-level device access may affect system stability.

---

_Contributions and bug reports are welcome!_
