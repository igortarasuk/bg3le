import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.listing.Function;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class FindModsettingsRefs extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modsettingslsx_refs.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            String[] needles = { "modsettings.lsx", "ModuleSettings", "_PROFILE_Public" };
            Memory mem = currentProgram.getMemory();

            for (String needle : needles) {
                out.println("=== Searching for string: " + needle + " ===");
                byte[] pattern = needle.getBytes("UTF-8");
                List<Address> hits = new ArrayList<>();
                for (MemoryBlock block : mem.getBlocks()) {
                    if (!block.isInitialized() || block.isExecute()) continue;
                    Address start = block.getStart();
                    Address end = block.getEnd();
                    Address searchFrom = start;
                    int hitCount = 0;
                    while (true) {
                        Address found = mem.findBytes(searchFrom, end, pattern, null, true, monitor);
                        if (found == null) break;
                        hits.add(found);
                        try { searchFrom = found.add(1); } catch (Exception e) { break; }
                        if (searchFrom.compareTo(end) >= 0) break;
                        if (++hitCount > 2000) break;
                    }
                }
                out.println("Found " + hits.size() + " occurrence(s)");
                ReferenceManager rm = currentProgram.getReferenceManager();
                for (Address strAddr : hits) {
                    List<Reference> refs = new ArrayList<>();
                    for (Reference r : rm.getReferencesTo(strAddr)) refs.add(r);
                    if (refs.isEmpty()) continue;
                    out.println("  string at " + strAddr + " refs=" + refs.size());
                    for (Reference r : refs) {
                        Address fromAddr = r.getFromAddress();
                        Function f = getFunctionContaining(fromAddr);
                        out.println("    ref from " + fromAddr + " in " + (f != null ? f.getName() + "@" + f.getEntryPoint() : "??? (no function)"));
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
