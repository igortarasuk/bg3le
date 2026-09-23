// Ghidra headless post-script (Java). See the retired find_ismodded.py in this
// same directory for the full rationale -- Ghidra 12.1.4 dropped bundled
// Jython, so this is a straight port to a GhidraScript instead of PyGhidra.
//
// Goal: locate the native-Linux equivalent of bg3se's ls::ModuleSettings::IsModded /
// esv::SavegameManager::HasCustomMods -- the check that gates achievements when
// mods are active. We already know six "official module GUID" FixedString globals
// built once at static-init time (see reference/ACHIEVEMENTS-DIAGNOSIS.md). Raw
// objdump+python xref scanning found only generic container/registration code
// referencing them. Ghidra's reference manager (which also resolves
// pointer-table / data-to-data refs a simple opcode scanner misses) and its
// decompiler give a much better shot at finding the real comparison.
//
// Output: plain text findings dumped to reference/ghidra_findings.txt, meant to
// be read and evaluated by a human/LLM afterwards -- this script does not try
// to judge which candidate is correct, only to surface all of them with
// decompiled pseudocode.

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
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.util.task.ConsoleTaskMonitor;

public class FindIsModded extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_findings.txt";

    // +0x100000: Ghidra's image base for this import is 0x100000, not 0 --
    // confirmed via CheckImageBase.java (objdump/gdb-derived raw file vaddrs
    // need this applied). The first run of this script used un-offset
    // addresses and so queried the wrong locations entirely.
    static final long IMAGE_BASE = 0x100000L;
    static final long[] TARGETS = {
        0x7d5af48L + IMAGE_BASE, 0x7d5af64L + IMAGE_BASE, 0x7d5af70L + IMAGE_BASE,
        0x7d5af68L + IMAGE_BASE, 0x7d25a38L + IMAGE_BASE, 0x7d26444L + IMAGE_BASE
    };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));

        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();

        out.println("bg3le achievements-gate hunt -- Ghidra xref + decompile dump");
        out.println("Program: " + currentProgram.getName());
        out.println("======================================================================");

        Map<Address, Set<Long>> funcHits = new LinkedHashMap<>();

        for (long t : TARGETS) {
            Address addr = space.getAddress(t);
            out.println();
            out.println(String.format("--- xrefs to 0x%x ---", t));
            ReferenceIterator refs = refMgr.getReferencesTo(addr);
            boolean any = false;
            while (refs.hasNext()) {
                Reference ref = refs.next();
                any = true;
                Address fromAddr = ref.getFromAddress();
                Function func = fm.getFunctionContaining(fromAddr);
                String fname = "???";
                Address fentry = null;
                if (func != null) {
                    fname = func.getName();
                    fentry = func.getEntryPoint();
                    funcHits.computeIfAbsent(fentry, k -> new LinkedHashSet<>()).add(t);
                }
                out.println("  " + fromAddr + "  in " + fname + " @ " + fentry
                    + "  (refType=" + ref.getReferenceType() + ")");
            }
            if (!any) {
                out.println("  (no references found)");
            }
        }

        out.println();
        out.println("======================================================================");
        out.println("Functions referencing the official-GUID globals, by hit count:");
        out.println("======================================================================");

        List<Map.Entry<Address, Set<Long>>> ranked = new ArrayList<>(funcHits.entrySet());
        ranked.sort((a, b) -> b.getValue().size() - a.getValue().size());

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (Map.Entry<Address, Set<Long>> e : ranked) {
            Address fentry = e.getKey();
            Set<Long> hits = e.getValue();
            Function func = fm.getFunctionAt(fentry);

            out.println();
            out.println("### " + func.getName() + " @ " + fentry + " -- " + hits.size()
                + "/" + TARGETS.length + " targets");
            StringBuilder hexHits = new StringBuilder();
            for (Long h : hits) hexHits.append(String.format("0x%x ", h));
            out.println("targets hit: " + hexHits.toString().trim());

            DecompileResults res = decomp.decompileFunction(func, 90, monitor);
            if (res.decompileCompleted()) {
                out.println("--- decompiled ---");
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("decompile FAILED: " + res.getErrorMessage());
            }
        }

        out.println();
        out.println("======================================================================");
        out.println("Done. " + funcHits.size() + " distinct functions reference at least one target.");
        out.close();

        println("find_ismodded: wrote " + OUT_PATH);
    }
}
