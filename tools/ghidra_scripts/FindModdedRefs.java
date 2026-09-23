import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class FindModdedRefs extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modded_refs.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            String[] needles = { "HasUnofficialMods", "Modded" };
            Memory mem = currentProgram.getMemory();

            for (String needle : needles) {
                out.println("=== Searching for string: " + needle + " ===");
                byte[] pattern = (needle + "\0").getBytes("UTF-8");
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
                        try {
                            searchFrom = found.add(1);
                        } catch (Exception e) {
                            break;
                        }
                        if (searchFrom.compareTo(end) >= 0) break;
                    }
                }
                out.println("Found " + hits.size() + " occurrence(s)");
                for (Address strAddr : hits) {
                    out.println("  string at " + strAddr);
                    ReferenceManager rm = currentProgram.getReferenceManager();
                    List<Reference> refs = new ArrayList<>();
                    for (Reference r : rm.getReferencesTo(strAddr)) {
                        refs.add(r);
                    }
                    out.println("    direct refs: " + refs.size());
                    for (Reference r : refs) {
                        Address fromAddr = r.getFromAddress();
                        Function f = getFunctionContaining(fromAddr);
                        out.println("      ref from " + fromAddr + " in function " + (f != null ? f.getName() + "@" + f.getEntryPoint() : "???"));
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
