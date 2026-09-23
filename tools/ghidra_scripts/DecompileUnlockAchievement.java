// Decompile the real, live-confirmed UnlockAchievement native handler
// (found via gdb breakpoint-chaining from the Osiris DIV dispatcher, see
// bg3le/reference/ACHIEVEMENTS-DIAGNOSIS.md) instead of guessing at xrefs.
// Static entry address 0x3088b90 (the real handler body; 0x3088b70 is a
// this-adjusting thunk that falls through to it). Unlike the earlier
// xref-to-GUID-globals search, this address is not a guess -- it is the
// exact function Osiris function id 0x80001669 dispatches to, confirmed
// live. If any static conditional distinguishes mods-on from mods-off, it
// has to be in this function or something it calls.

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileUnlockAchievement extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_unlockachievement.txt";

    // +0x100000: Ghidra's image base for this import is 0x100000, not 0 --
    // confirmed via CheckImageBase.java. All addresses gathered live (gdb,
    // objdump) are raw file vaddrs and need this offset applied before they
    // mean anything in Ghidra's address space.
    static final long IMAGE_BASE = 0x100000L;
    static final long HANDLER_ENTRY = 0x3088b90L + IMAGE_BASE;
    static final long THUNK_ENTRY = 0x3088b70L + IMAGE_BASE;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        FunctionManager fm = currentProgram.getFunctionManager();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (long t : new long[]{THUNK_ENTRY, HANDLER_ENTRY}) {
            Address addr = space.getAddress(t);
            out.println("======================================================================");
            out.println(String.format("Address 0x%x", t));
            out.println("======================================================================");

            Function func = fm.getFunctionContaining(addr);
            if (func == null) {
                out.println("No Function object contains this address.");
                out.println("Attempting disassembly-only listing of 200 bytes from here:");
                Address cur = addr;
                for (int i = 0; i < 60 && cur != null; i++) {
                    Instruction ins = currentProgram.getListing().getInstructionAt(cur);
                    if (ins == null) { out.println("  (no instruction at " + cur + ")"); break; }
                    out.println("  " + cur + "  " + ins);
                    cur = ins.getFallThrough();
                }
                out.println();
                continue;
            }

            out.println("Function: " + func.getName() + " @ " + func.getEntryPoint());
            out.println("Body ranges:");
            func.getBody().forEach(r -> out.println("  " + r));

            long instrCount = 0;
            InstructionIterator it = currentProgram.getListing().getInstructions(func.getBody(), true);
            while (it.hasNext()) { it.next(); instrCount++; }
            out.println("Instruction count: " + instrCount);

            out.println();
            out.println("-- decompiling with a 300s timeout --");
            long t0 = System.currentTimeMillis();
            DecompileResults res = decomp.decompileFunction(func, 300, monitor);
            long elapsed = System.currentTimeMillis() - t0;
            out.println("elapsed: " + elapsed + " ms");
            out.println("decompileCompleted(): " + res.decompileCompleted());
            out.println("getErrorMessage(): [" + res.getErrorMessage() + "]");
            if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                out.println("--- decompiled C ---");
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("Decompilation did not complete; falling back to a full");
                out.println("disassembly listing of the function body instead:");
                InstructionIterator it2 = currentProgram.getListing().getInstructions(func.getBody(), true);
                while (it2.hasNext()) {
                    Instruction ins = it2.next();
                    out.println("  " + ins.getAddress() + "  " + ins);
                }
            }
            out.println();
        }

        out.close();
        println("DecompileUnlockAchievement: wrote " + OUT_PATH);
    }
}
