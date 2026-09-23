import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DecompileModListAndAchRefs extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modlist_and_achrefs.txt";
    // VMA (from objdump/readelf) + 0xFF000 == established Ghidra address convention for .text
    static final long MODLIST_FUNC_VMA = 0x7538ef0L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            long ghidraAddr = MODLIST_FUNC_VMA + 0xFF000L;
            Address addr = toAddr(ghidraAddr);
            out.println("=== Address check: VMA 0x" + Long.toHexString(MODLIST_FUNC_VMA)
                    + " -> ghidra 0x" + Long.toHexString(ghidraAddr) + " ===");

            Instruction ins = getInstructionAt(addr);
            if (ins == null) {
                disassemble(addr);
                ins = getInstructionAt(addr);
            }
            out.println("Instruction at that address: " + (ins != null ? ins.toString() : "NONE"));
            // dump a few instructions either side to eyeball against known objdump bytes
            Address cur = addr;
            for (int i = 0; i < 10 && cur != null; i++) {
                Instruction cins = getInstructionAt(cur);
                if (cins == null) { disassemble(cur); cins = getInstructionAt(cur); }
                out.println("  " + cur + ": " + (cins != null ? cins.toString() : "??"));
                cur = cins != null ? cins.getMaxAddress().add(1) : null;
            }

            Function f = getFunctionContaining(addr);
            if (f == null) {
                out.println("No function contains this address; trying createFunction");
                f = createFunction(addr, null);
            }
            out.println("Function: " + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "still none"));

            if (f != null) {
                DecompInterface decomp = new DecompInterface();
                decomp.openProgram(currentProgram);
                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println("--- DECOMPILED ---");
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                }
            }

            out.println();
            out.println("=== UnlockAchievement string xref retry (post full-analysis) ===");
            Memory mem = currentProgram.getMemory();
            byte[] pattern = "UnlockAchievement".getBytes("UTF-8");
            List<Address> hits = new ArrayList<>();
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized()) continue;
                Address start = block.getStart();
                Address end = block.getEnd();
                Address searchFrom = start;
                while (true) {
                    Address found = mem.findBytes(searchFrom, end, pattern, null, true, monitor);
                    if (found == null) break;
                    hits.add(found);
                    try { searchFrom = found.add(1); } catch (Exception e) { break; }
                    if (searchFrom.compareTo(end) >= 0) break;
                }
            }
            out.println("Found " + hits.size() + " occurrence(s)");
            ReferenceManager rm = currentProgram.getReferenceManager();
            for (Address strAddr : hits) {
                MemoryBlock blk = mem.getBlock(strAddr);
                out.println("string at " + strAddr + " block=" + (blk != null ? blk.getName() : "?"));
                for (Reference r : rm.getReferencesTo(strAddr)) {
                    Address fromAddr = r.getFromAddress();
                    Function rf = getFunctionContaining(fromAddr);
                    out.println("  ref from " + fromAddr + " type=" + r.getReferenceType() + " in "
                            + (rf != null ? rf.getName() + "@" + rf.getEntryPoint() : "??? (no function)"));
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
