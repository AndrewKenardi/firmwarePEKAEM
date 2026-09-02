#ifndef FW_VERSION_H
#define FW_VERSION_H

// Update angka ini setiap kali build firmware baru untuk slave.
// Master akan mengirim angka ini, dan slave akan membandingkan
// dengan nilai yang sama yang di-compile ke dalam dirinya sendiri.
#define FIRMWARE_VERSION   0x00010004UL   // format: 0x00MMmmpp (Major.Minor.Patch)

#endif // FW_VERSION_H