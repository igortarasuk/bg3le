import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class FindCallers extends GhidraScript {
    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modcheck_candidate.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager refMgr = currentProgram.getReferenceManager();

        Address target = space.getAddress(0x02f06590L);
        out.println("Callers of FUN_02f06590:");
        ReferenceIterator refs = refMgr.getReferencesTo(target);
        while (refs.hasNext()) {
            Reference ref = refs.next();
            Address from = ref.getFromAddress();
            Function f = fm.getFunctionContaining(from);
            out.println("  " + from + "  in " + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "???")
                + "  refType=" + ref.getReferenceType());
        }

        out.println();
        out.println("Callers of those callers (2 levels up), deduped:");
        java.util.LinkedHashSet<Address> callerFuncs = new java.util.LinkedHashSet<>();
        ReferenceIterator refs2 = refMgr.getReferencesTo(target);
        while (refs2.hasNext()) {
            Reference ref = refs2.next();
            Function f = fm.getFunctionContaining(ref.getFromAddress());
            if (f != null) callerFuncs.add(f.getEntryPoint());
        }
        for (Address callerEntry : callerFuncs) {
            out.println("--- callers of " + callerEntry + " ---");
            ReferenceIterator r3 = refMgr.getReferencesTo(callerEntry);
            while (r3.hasNext()) {
                Reference ref = r3.next();
                Address from = ref.getFromAddress();
                Function f = fm.getFunctionContaining(from);
                out.println("    " + from + "  in " + (f != null ? f.getName() + " @ " + f.getEntryPoint() : "???"));
            }
        }

        out.close();
        println("FindCallers: wrote " + OUT_PATH);
    }
}
