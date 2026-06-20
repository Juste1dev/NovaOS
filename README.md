# NovaOS

NovaOS is an experimental operating system built from scratch.
It aims to build a complete OS including kernel, drivers, GUI, networking, and an integrated application ecosystem.

This is an educational and technical project focused on deep understanding of low-level systems.

---

# Project Structure

```
NovaOS/
├── NovaOS_6.0.iso
└── source_code/
    kernel.c
    libc.c
    libc.h
    linker.ld
    Makefile
    pthread.h
    stdlib.h

    apps/
        browser.c
        calculator.c
        editor.c
        filemanager.c
        installer.c
        nova_pkg.c / nova_pkg.h
        shell_ext.c / shell_ext.h
        symera.c / symera.h
        terminal.c
        apps_misc.c

    assets/
        wallpapers/
            wp00.bin → wp15.bin

    boot/
        boot.asm

    drivers/
        driver_manager.c / .h
        keyboard.c / .h
        mouse.c / .h
        pci.c / .h
        sound.c / .h
        vbe.c / .h

    fs/
        vfs.c / vfs.h

    gui/
        gui.c / gui.h
        font.c / font.h
        wayland.c / wayland.h
        custom_assets.c / .h
        user_wallpapers.c / .h

    kernel/
        gdt.c / gdt.h / gdt_flush.asm
        idt.c / idt.h
        isr.asm
        memory.c / memory.h
        syscalls.c / syscalls.h
        timer.c / timer.h
        users.c / users.h
        userspace.c / userspace.h
        module_loader.c / module_loader.h
        platform_features.c / .h
        ssh.c / ssh.h
        stack_protector.c

        elf/
        init/
        ipc/
        memory/
        pthread/
        sched/
        sync/

    net/
        net.c / net.h
        stack.c / stack.h
        webcache.h

    store/
        index.json
        packages/
            org.nova.*
            org.mozilla.firefox.nova
```

---

# Kernel

The NovaOS kernel is the core of the system.

### Main features:

* Interrupt handling (IDT, ISR)
* Memory management (heap, paging, VMM, PMM)
* Scheduling system
* System calls and user space
* Thread support (pthread runtime)
* Inter-process communication (IPC)
* Low-level security (stack protector)

---

# Drivers

NovaOS includes a modular driver system:

* Keyboard and mouse input
* PCI device detection
* Audio (sound system)
* Display via VBE
* Centralized driver manager

---

# GUI

The graphical system includes:

* Low-level rendering engine
* Font system
* Custom asset handling
* User wallpapers support
* Wayland-like abstraction layer

---

# Applications

NovaOS ships with built-in system applications:

* Web browser
* Terminal
* File manager
* Calculator
* Code editor
* System installer
* Shell extensions
* Package system (nova_pkg)
* AI assistant (Symera)

---

# File System

* Virtual File System (VFS)
* Package support via store
* JSON-based app index

---

# Networking

* Low-level networking stack
* Internal network API
* Web cache system (webcache)

---

# Boot

* Bootloader written in Assembly
* Multi-phase kernel initialization
* Early console + panic system

---

# Assets

* System wallpapers included
* Optimized binary format (.bin)

---

# Project Status

NovaOS is currently in active development:

* Kernel functional but evolving
* GUI under active construction
* Drivers partially stable
* System apps already implemented
* Networking in development

---

# Tech Stack

* C (kernel + userland)
* Assembly (boot + interrupts)
* Custom linker script (linker.ld)
* VBE framebuffer graphics
* Modular architecture

---

# Roadmap

* Kernel stabilization
* Scheduler improvements
* GUI completion
* Full filesystem implementation
* Advanced networking
* Application sandboxing
* Driver optimization
* Stable store interface

---

# Vision

NovaOS aims to become a full experimental operating system combining low-level understanding with a modern user experience.

---

# Note

This project is experimental and continuously evolving. Instability is expected.
