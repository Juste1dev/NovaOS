NovaOS

NovaOS est un système d’exploitation expérimental développé from scratch.
Il vise à construire un OS complet avec kernel, drivers, GUI, réseau et système d’applications intégré.

Projet éducatif et technique orienté compréhension profonde des systèmes bas niveau.

Architecture du projet
NovaOS/
├── NovaOS_6.0.iso
└── source_code/
    │   kernel.c
    │   libc.c
    │   libc.h
    │   linker.ld
    │   Makefile
    │   pthread.h
    │   stdlib.h
    │
    ├── apps/
    │   browser.c
    │   calculator.c
    │   editor.c
    │   filemanager.c
    │   installer.c
    │   nova_pkg.c / nova_pkg.h
    │   shell_ext.c / shell_ext.h
    │   symera.c / symera.h
    │   terminal.c
    │   apps_misc.c
    │
    ├── assets/
    │   └── wallpapers/
    │       wp00.bin → wp15.bin
    │
    ├── boot/
    │   boot.asm
    │
    ├── drivers/
    │   driver_manager.c / .h
    │   keyboard.c / .h
    │   mouse.c / .h
    │   pci.c / .h
    │   sound.c / .h
    │   vbe.c / .h
    │
    ├── fs/
    │   vfs.c / vfs.h
    │
    ├── gui/
    │   gui.c / gui.h
    │   font.c / font.h
    │   wayland.c / wayland.h
    │   custom_assets.c / .h
    │   user_wallpapers.c / .h
    │
    ├── kernel/
    │   gdt.c / gdt.h / gdt_flush.asm
    │   idt.c / idt.h
    │   isr.asm
    │   memory.c / memory.h
    │   syscalls.c / syscalls.h
    │   timer.c / timer.h
    │   users.c / users.h
    │   userspace.c / userspace.h
    │   module_loader.c / module_loader.h
    │   platform_features.c / .h
    │   ssh.c / ssh.h
    │   stack_protector.c
    │
    │   ├── elf/
    │   ├── init/
    │   ├── ipc/
    │   ├── memory/
    │   ├── pthread/
    │   ├── sched/
    │   └── sync/
    │
    ├── net/
    │   net.c / net.h
    │   stack.c / stack.h
    │   webcache.h
    │
    └── store/
        index.json
        packages/
            org.nova.*
            org.mozilla.firefox.nova
Kernel

Le kernel est le cœur de NovaOS.

Fonctionnalités principales :

Gestion des interruptions (IDT, ISR)
Gestion mémoire (heap, paging, VMM, PMM)
Système de scheduling (scheduler)
Syscalls et espace utilisateur
Support threads (pthread runtime)
IPC (communication inter-processus)
Sécurité bas niveau (stack protector)
Drivers

NovaOS intègre un système de drivers modulaire :

clavier et souris
PCI device detection
audio (sound)
affichage via VBE
gestion centralisée via driver manager
GUI

Le système graphique comprend :

rendu bas niveau
système de fonts
gestion d’assets personnalisés
wallpapers utilisateurs
base type Wayland-like (abstraction GUI)
Applications

NovaOS embarque un écosystème d’apps internes :

navigateur
terminal
file manager
calculator
editor
installer système
shell extensions
système de packages (nova_pkg)
assistant Symera
File system
VFS (Virtual File System)
support packages via store
index JSON pour gestion des apps
Réseau
stack réseau bas niveau
API réseau interne
système de cache web (webcache)
Boot
bootloader en ASM
initialisation kernel via phases
early console + panic system
Assets
wallpapers intégrés au système
format binaire optimisé (.bin)
État du projet

NovaOS est en phase avancée de développement :

kernel fonctionnel mais en évolution
GUI en construction active
drivers partiellement stables
apps système déjà présentes
networking en développement
Stack technique
C (kernel + userland)
Assembly (boot + interruptions)
linker script (linker.ld)
framebuffer VBE
architecture modulaire custom
Roadmap
stabilisation kernel
amélioration scheduler
finalisation GUI
file system complet
réseau avancé
sandbox applications
optimisation drivers
interface store stable
Vision

NovaOS vise à devenir un OS complet expérimental, combinant compréhension bas niveau et expérience utilisateur moderne.

Remarque

Projet en constante évolution, expérimental et instable par nature.
