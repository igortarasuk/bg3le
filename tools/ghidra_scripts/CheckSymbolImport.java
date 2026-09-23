import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

import java.io.FileWriter;
import java.io.PrintWriter;

public class CheckSymbolImport extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_symbol_check.txt";

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            // Known-good symbols from `nm bg3` to check if Ghidra imported real names
            String[] names = {
                "__cxa_begin_catch",
                "_ZN6Noesis14BaseCollection3AddEPNS_13BaseComponentE",
                "_ZSt9terminatev",
                "__clang_call_terminate"
            };
            SymbolTable st = currentProgram.getSymbolTable();
            for (String n : names) {
                SymbolIterator it = st.getSymbols(n);
                boolean found = false;
                while (it.hasNext()) {
                    Symbol s = it.next();
                    out.println(n + " -> " + s.getAddress() + " (type=" + s.getSymbolType() + ")");
                    found = true;
                }
                if (!found) out.println(n + " -> NOT FOUND in Ghidra symbol table");
            }

            out.println();
            out.println("=== total function count ===");
            FunctionManager fm = currentProgram.getFunctionManager();
            out.println(fm.getFunctionCount());

            out.println();
            out.println("=== search for LoadProtocol/ModuleSettings/CacheModList in Ghidra function names ===");
            for (Function f : fm.getFunctions(true)) {
                String nm = f.getName();
                if (nm.contains("LoadProtocol") || nm.contains("ModuleSettings")
                        || nm.contains("CacheModList") || nm.contains("SavegameManager")
                        || nm.contains("HasCustomMods")) {
                    out.println(f.getName() + " @ " + f.getEntryPoint());
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
