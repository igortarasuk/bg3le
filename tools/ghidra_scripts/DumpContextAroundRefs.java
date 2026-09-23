import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpContextAroundRefs extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_context_around_refs.txt";
    static final long[] REF_ADDRS = { 0x04075fa9L, 0x07a16998L };
    static final long BEFORE = 0x400L;
    static final long AFTER = 0x300L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            for (long refLong : REF_ADDRS) {
                Address refAddr = toAddr(refLong);
                out.println("=== ref address " + refAddr + " ===");

                // Find nearest function whose entry <= refAddr (search backward)
                FunctionIterator backIter = currentProgram.getListing().getFunctions(refAddr, false);
                Function nearestBefore = backIter.hasNext() ? backIter.next() : null;
                out.println("Nearest function at/before ref: " +
                        (nearestBefore != null ? nearestBefore.getName() + " @ " + nearestBefore.getEntryPoint() +
                                " (body end " + nearestBefore.getBody().getMaxAddress() + ")" : "none"));

                FunctionIterator fwdIter = currentProgram.getListing().getFunctions(refAddr, true);
                Function nearestAfter = fwdIter.hasNext() ? fwdIter.next() : null;
                out.println("Nearest function at/after ref: " +
                        (nearestAfter != null ? nearestAfter.getName() + " @ " + nearestAfter.getEntryPoint() : "none"));

                // Ensure the window is disassembled as best-effort (ignore errors)
                Address winStart = refAddr.subtract(BEFORE);
                Address winEnd = refAddr.add(AFTER);
                try {
                    DisassembleCommand dis = new DisassembleCommand(winStart, null, true);
                    dis.applyTo(currentProgram, monitor);
                } catch (Exception e) {
                    out.println("disassemble attempt failed: " + e.getMessage());
                }

                out.println("--- raw instruction listing from " + winStart + " to " + winEnd + " ---");
                Address cur = winStart;
                int count = 0;
                while (cur.compareTo(winEnd) <= 0 && count < 4000) {
                    Instruction ins = getInstructionAt(cur);
                    if (ins != null) {
                        String marker = cur.equals(refAddr) ? "  <== REF" : "";
                        out.println(cur + ":  " + ins.toString() + marker);
                        cur = ins.getMaxAddress().add(1);
                    } else {
                        byte b = 0;
                        try { b = getByte(cur); } catch (Exception e) {}
                        out.println(cur + ":  db 0x" + String.format("%02x", b & 0xff) + " (no instruction)");
                        cur = cur.add(1);
                    }
                    count++;
                }

                // If nearestBefore function exists and its body doesn't reach refAddr, decompile it anyway for context
                if (nearestBefore != null) {
                    out.println("--- decompile of nearestBefore function (" + nearestBefore.getName() + ") ---");
                    DecompInterface decomp = new DecompInterface();
                    decomp.openProgram(currentProgram);
                    DecompileResults res = decomp.decompileFunction(nearestBefore, 60, monitor);
                    if (res != null && res.decompileCompleted()) {
                        out.println(res.getDecompiledFunction().getC());
                    } else {
                        out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                    }
                }
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
