import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;

public class InspectNewRefs extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_inspect_newrefs.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            dumpArea(out, 0x021876c0L, 0x60, "vtable-ish area around 0x021876dc");
            dumpArea(out, 0x00cac600L, 0x80, "data table area around 0x00cac628");

            // who points AT 0x021876dc itself (i.e. who uses this vtable)?
            out.println();
            out.println("=== refs TO 0x021876dc (the vtable slot) ===");
            ReferenceManager rm = currentProgram.getReferenceManager();
            for (Reference r : rm.getReferencesTo(toAddr(0x021876dcL))) {
                Address fromAddr = r.getFromAddress();
                Function caller = getFunctionContaining(fromAddr);
                out.println("  ref from " + fromAddr + " type=" + r.getReferenceType() + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "??? (no function)"));
            }
            // does anything point at the CONTAINING object of this vtable (i.e. at 0x021876c0-ish, the vtable start)?
            out.println();
            out.println("=== refs TO 0x00cac628 (the data table slot) ===");
            for (Reference r : rm.getReferencesTo(toAddr(0x00cac628L))) {
                Address fromAddr = r.getFromAddress();
                Function caller = getFunctionContaining(fromAddr);
                out.println("  ref from " + fromAddr + " type=" + r.getReferenceType() + " in " + (caller != null ? caller.getName() + "@" + caller.getEntryPoint() : "??? (no function)"));
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }

    private void dumpArea(PrintWriter out, long start, int len, String label) throws Exception {
        out.println("--- " + label + " ---");
        Address a = toAddr(start);
        MemoryBlock block = currentProgram.getMemory().getBlock(a);
        out.println("block=" + (block != null ? block.getName() : "null"));
        for (int i = 0; i < len; i += 8) {
            long val = 0;
            try { val = getLong(a.add(i)); } catch (Exception e) { out.println((a.add(i)) + ": <unreadable>"); continue; }
            out.println((a.add(i)) + ": 0x" + Long.toHexString(val));
        }
        out.println();
    }
}
