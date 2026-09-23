// Sanity-check Ghidra's addressing convention against objdump's for this
// binary. If they don't match 1:1, every static address used throughout
// ACHIEVEMENTS-DIAGNOSIS.md (found via live gdb bias subtraction and
// verified against plain `objdump -d bin/bg3`) needs a fixed offset applied
// before it means anything in Ghidra's address space.

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class CheckImageBase extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_imagebase_check.txt";

    // Known-good, live-confirmed bytes at raw-file address 0x3088b90 (per
    // objdump -d bin/bg3 --start-address=0x3088b90, and confirmed live via
    // gdb): 55 41 57 41 56 41 55 41 54 53 48 83 ec 58
    // (push rbp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x58)

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));

        out.println("Program image base: " + currentProgram.getImageBase());
        out.println("Language: " + currentProgram.getLanguage().getLanguageID());
        out.println();
        out.println("Memory blocks:");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            out.println(String.format("  %-20s start=%s end=%s size=0x%x perms=%s%s%s",
                b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-"));
        }
        out.println();

        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();

        long[] probes = {0x3088b90L, 0x3088bb1L, 0x2cd4500L};
        for (long p : probes) {
            Address addr = space.getAddress(p);
            out.println("---- Ghidra address " + addr + " ----");
            byte[] bytes = new byte[16];
            try {
                currentProgram.getMemory().getBytes(addr, bytes);
                StringBuilder sb = new StringBuilder();
                for (byte bb : bytes) sb.append(String.format("%02x ", bb));
                out.println("  raw bytes: " + sb);
            } catch (Exception e) {
                out.println("  raw bytes: <unreadable: " + e + ">");
            }
            Instruction ins = currentProgram.getListing().getInstructionAt(addr);
            out.println("  instruction here: " + (ins != null ? ins.toString() : "(none)"));
        }

        out.close();
        println("CheckImageBase: wrote " + OUT_PATH);
    }
}
