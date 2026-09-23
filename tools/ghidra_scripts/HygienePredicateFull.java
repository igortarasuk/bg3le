import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

// Persisted: clears bogus noreturn flags, repairs bodies, decompiles.
public class HygienePredicateFull extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_predicate_full.txt";
    static final long[] EXPLICIT_CLEAR = {0x07ed7090L, 0x02377170L, 0x02480220L, 0x02538520L, 0x0398e8e0L,
                                          0x02537b60L, 0x0239fa70L, 0x02554490L};
    // Pairs: address of interest, guessed entry (0 = derive).
    static final long[][] TARGETS = {
        {0x038675f0L, 0}, {0x03867580L, 0}, {0x02538520L, 0}, {0x0239fa70L, 0}, {0x02537b60L, 0},
        {0x03867780L, 0}, {0x03867800L, 0}, {0x03867230L, 0}, {0x04119c40L, 0}, {0x02d2cb20L, 0},
        {0x0460c8d0L, 0}, {0x027ab5d0L, 0}, {0x05209d00L, 0}, {0x0761d440L, 0}, {0x0398e8e0L, 0},
        {0x063be2caL, 0x063be100L}, {0x063c04f3L, 0x063bf250L}, {0x051db42cL, 0x051daf40L},
        {0x051db4f9L, 0x051daf40L}, {0x06386239L, 0x063815e0L}, {0x06ff93a0L, 0x06ff8bd0L},
    };
    static final String[] STRINGS = {"eocnet::AchievementMessage", "eocnet::NETMSG_ACHIEVEMENT_UNLOCKED_MESSAGE",
        "eocnet::AchievementProgressMessage", "eocnet::NETMSG_ACHIEVEMENT_PROGRESS_MESSAGE", "BG3_Quest01",
        "STEAMUSERSTATS_INTERFACE_VERSION012", "SetAchievementProgress", "UnlockAchievement"};
    static final String[] LIBC_PREFIX = {"_ZSt", "_ZNSt", "_ZNKSt", "_Znwm", "_Znam", "_ZdlPv", "_ZdaPv", "__cxa_",
        "__gxx_", "_Unwind_", "__clang_call_terminate", "_ZTv0_", "__stack_chk", "__assert", "__libc", "_ZThn",
        "__cxx_", "__tls_get_addr", "pthread_", "__pthread", "__errno", "__sigsetjmp", "__memcpy_chk",
        "__strcpy_chk", "__snprintf_chk", "__sprintf_chk", "__printf_chk", "__vsnprintf_chk", "_ZN9__gnu_cxx"};
    static final Set<String> LIBC_NAMES = new HashSet<>(Arrays.asList(
        "strlen", "memcpy", "memmove", "memset", "memcmp", "strcmp", "strncmp", "strcpy", "strncpy", "strchr",
        "strrchr", "strstr", "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc",
        "atexit", "snprintf", "sprintf", "vsnprintf", "printf", "fprintf", "vfprintf", "puts",
        "fputs", "fwrite", "fread", "fopen", "fclose", "fflush", "read", "write", "open", "close", "mmap",
        "munmap", "mprotect", "getenv", "setenv", "strtol", "strtoul", "strtoull", "strtod", "atoi", "atof",
        "qsort", "bsearch", "sin", "cos", "tan", "sqrt", "pow", "exp", "log", "floor", "ceil", "fmod",
        "sinf", "cosf", "tanf", "sqrtf", "powf", "expf", "logf", "floorf", "ceilf", "fmodf", "atan2", "atan2f",
        "usleep", "nanosleep", "sleep", "clock_gettime", "gettimeofday", "time", "localtime_r", "gmtime_r",
        "strftime", "mktime", "select", "poll", "socket", "connect", "send", "recv", "sendto", "recvfrom",
        "toupper", "tolower", "isalpha", "isdigit", "isspace", "isalnum", "wcslen", "wcscmp", "mbstowcs",
        "wcstombs", "strdup", "strndup", "strerror", "perror", "dlopen", "dlsym", "dlclose", "dlerror",
        "sysconf", "getpid", "gettid", "sched_yield", "syscall", "ioctl", "fcntl", "stat", "fstat", "lstat",
        "access", "unlink", "rename", "mkdir", "rmdir", "opendir", "readdir", "closedir", "getcwd", "chdir",
        "readlink", "realpath", "fork", "execv", "execve", "waitpid", "kill", "signal", "sigaction", "raise",
        "setjmp", "__errno_location", "fseek", "ftell", "fseeko", "ftello", "fgets", "fgetc",
        "getc", "ungetc", "setvbuf", "ferror", "feof", "clearerr", "remove", "tmpfile", "fileno", "dup", "dup2",
        "pipe", "uname", "getuid", "geteuid", "getpwuid_r", "gethostname", "getaddrinfo", "freeaddrinfo",
        "inet_ntop", "inet_pton", "htons", "ntohs", "htonl", "ntohl", "setsockopt", "getsockopt", "bind",
        "listen", "accept", "shutdown", "getsockname", "getpeername", "memchr", "memrchr", "strcasecmp",
        "strncasecmp", "strspn", "strcspn", "strpbrk", "strtok", "strtok_r", "sscanf", "vsscanf", "fscanf",
        "iconv", "iconv_open", "iconv_close", "setlocale", "localeconv", "newlocale", "uselocale", "freelocale",
        "wctob", "btowc", "wcrtomb", "mbrtowc", "wmemcpy", "wmemmove", "wmemset", "wmemcmp", "wmemchr",
        "wcsnrtombs", "mbsnrtowcs", "wcsftime", "wcscoll", "wcsxfrm", "strcoll", "strxfrm", "towupper",
        "towlower", "iswalpha", "iswdigit", "iswspace", "iswalnum", "iswpunct", "iswupper", "iswlower",
        "iswcntrl", "iswprint", "iswxdigit", "iswblank", "ispunct", "isupper", "islower", "iscntrl", "isprint",
        "isxdigit", "isblank", "isgraph", "iswgraph", "getrusage", "getrlimit", "setrlimit", "sched_getaffinity",
        "sched_setaffinity", "sem_wait", "sem_post", "sem_init", "sem_destroy", "sem_timedwait", "shm_open",
        "shm_unlink", "ftruncate", "msync", "madvise", "sigemptyset", "sigaddset", "sigprocmask", "backtrace",
        "backtrace_symbols", "backtrace_symbols_fd", "dl_iterate_phdr", "dladdr"));
    static final Set<String> TRULY_NORETURN = new HashSet<>(Arrays.asList("abort", "exit", "_exit", "__stack_chk_fail",
        "__cxa_throw", "__cxa_rethrow", "__cxa_pure_virtual", "__cxa_deleted_virtual", "__cxa_bad_cast",
        "__cxa_bad_typeid", "__cxa_call_unexpected", "_Unwind_Resume", "__clang_call_terminate", "_ZSt9terminatev",
        "_ZSt17__throw_bad_allocv", "longjmp", "__assert_fail", "quick_exit", "_ZSt25__throw_bad_function_callv",
        "_ZSt20__throw_length_errorPKc", "_ZSt24__throw_out_of_rangePKc", "_ZSt19__throw_logic_errorPKc",
        "_ZSt20__throw_out_of_range_fmtPKcz", "_ZSt16__throw_bad_castv", "_ZSt24__throw_invalid_argumentPKc",
        "_ZSt21__throw_runtime_errorPKc", "_ZSt20__throw_system_errori", "_ZSt19__throw_domain_errorPKc",
        "_ZSt21__throw_overflow_errorPKc", "_ZSt22__throw_underflow_errorPKc", "_ZSt19__throw_range_errorPKc",
        "_ZSt23__throw_future_errori", "_ZSt18__throw_bad_arg_ptrv"));

    DecompInterface decomp;
    PrintWriter out;
    Set<Address> decompiled = new HashSet<>();
    Set<Address> clearedEntries = new LinkedHashSet<>();

    String baseName(String n) {
        int i = n.indexOf('@');
        return i > 0 ? n.substring(0, i) : n;
    }

    boolean isLibcLike(Function f) {
        String n = baseName(f.getName());
        if (f.isThunk() || f.isExternal()) return true;
        if (LIBC_NAMES.contains(n)) return true;
        for (String p : LIBC_PREFIX) if (n.startsWith(p)) return true;
        return false;
    }

    boolean hasRet(Function f) {
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType().isTerminal() && ins.getMnemonicString().toUpperCase().startsWith("RET")) return true;
        }
        return false;
    }

    void hygiene() throws Exception {
        out.println("=================================================================");
        out.println("=== NORETURN HYGIENE (persisted) ===");
        int total = 0, libc = 0, explicit = 0, retfun = 0, kept = 0;
        FunctionIterator fit = currentProgram.getFunctionManager().getFunctions(true);
        List<String> clearedNames = new ArrayList<>();
        List<String> keptNames = new ArrayList<>();
        Set<Long> explicitSet = new HashSet<>();
        for (long a : EXPLICIT_CLEAR) explicitSet.add(a);
        while (fit.hasNext()) {
            Function f = fit.next();
            if (!f.hasNoReturn()) continue;
            total++;
            String n = baseName(f.getName());
            String why = null;
            if (TRULY_NORETURN.contains(n)) { kept++; keptNames.add(f.getEntryPoint() + " " + n); continue; }
            if (explicitSet.contains(f.getEntryPoint().getOffset())) { why = "explicit"; explicit++; }
            else if (isLibcLike(f)) { why = "libc/thunk"; libc++; }
            else if (n.startsWith("FUN_") && hasRet(f)) { why = "FUN_ with RET in body"; retfun++; }
            if (why == null) { kept++; keptNames.add(f.getEntryPoint() + " " + n); continue; }
            f.setNoReturn(false);
            clearedEntries.add(f.getEntryPoint());
            clearedNames.add(f.getEntryPoint() + " " + f.getName() + " [" + why + "]");
        }
        for (long a : EXPLICIT_CLEAR) {
            Function f = getFunctionAt(toAddr(a));
            if (f != null) { if (f.hasNoReturn()) f.setNoReturn(false); clearedEntries.add(f.getEntryPoint()); }
            else out.println("  explicit clear target has no function: " + toAddr(a));
        }
        ExternalManager em = currentProgram.getExternalManager();
        for (String lib : em.getExternalLibraryNames()) {
            Iterator<ExternalLocation> eit = em.getExternalLocations(lib);
            while (eit.hasNext()) {
                ExternalLocation el = eit.next();
                Function ef = el.getFunction();
                if (ef != null && ef.hasNoReturn() && !TRULY_NORETURN.contains(baseName(ef.getName()))) {
                    ef.setNoReturn(false); libc++; clearedNames.add("external " + ef.getName() + " [libc/thunk]");
                }
            }
        }
        out.println("noreturn functions total=" + total + " cleared: libc/thunk=" + libc + " explicit=" + explicit
                + " FUN_withRET=" + retfun + " kept=" + kept);
        for (String s : clearedNames) out.println("  cleared " + s);
        out.println("kept (still noreturn): " + keptNames.size());
        for (String s : keptNames) out.println("  kept " + s);
        int overrides = 0, sites = 0;
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Address e : clearedEntries) {
            ReferenceIterator rit = rm.getReferencesTo(e);
            while (rit.hasNext()) {
                Reference r = rit.next();
                if (!r.getReferenceType().isCall()) continue;
                sites++;
                Instruction ins = getInstructionAt(r.getFromAddress());
                if (ins != null && ins.getFlowOverride() == FlowOverride.CALL_RETURN) {
                    ins.setFlowOverride(FlowOverride.NONE); overrides++;
                }
            }
        }
        out.println("call sites of cleared functions=" + sites + " CALL_RETURN overrides removed=" + overrides);
        out.println();
    }

    // Fill reachable disassembly gaps, then recreate the function body.
    Function repair(Address entry) throws Exception {
        Function f = getFunctionAt(entry);
        if (f == null) {
            disassemble(entry);
            CreateFunctionCmd c = new CreateFunctionCmd(entry);
            c.applyTo(currentProgram, monitor);
            f = getFunctionAt(entry);
            if (f == null) { out.println("  could not create function at " + entry + ": " + c.getStatusMsg()); return null; }
            out.println("  created function " + f.getName() + " at " + entry);
        }
        long before = f.getBody().getNumAddresses();
        for (int pass = 0; pass < 12; pass++) {
            boolean changed = false;
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            List<Address> gaps = new ArrayList<>();
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getFlowOverride() == FlowOverride.CALL_RETURN) {
                    boolean calleeReturns = true;
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function cf = getFunctionAt(r.getToAddress());
                        if (cf != null && cf.hasNoReturn()) calleeReturns = false;
                    }
                    if (calleeReturns) { ins.setFlowOverride(FlowOverride.NONE); changed = true; }
                }
                if (ins.getFlowType().isCall() && ins.getFlowOverride() != FlowOverride.CALL_RETURN) {
                    boolean noret = false;
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function cf = getFunctionAt(r.getToAddress());
                        if (cf != null && cf.hasNoReturn()) noret = true;
                    }
                    if (!noret) {
                        Address ft = ins.getMaxAddress().add(1);
                        if (getInstructionAt(ft) == null && getUndefinedDataAt(ft) != null && (getByte(ft) & 0xff) != 0xcc) gaps.add(ft);
                    }
                }
                if (!ins.getFlowType().isCall()) {
                    for (Address d : ins.getFlows()) {
                        if (d == null || !currentProgram.getMemory().contains(d)) continue;
                        if (getInstructionAt(d) == null && getUndefinedDataAt(d) != null) gaps.add(d);
                    }
                }
            }
            for (Address g : gaps) { if (getInstructionAt(g) == null) { disassemble(g); changed = true; } }
            if (!changed) break;
            CreateFunctionCmd cmd = new CreateFunctionCmd(null, entry, null, SourceType.DEFAULT, false, true);
            boolean ok = cmd.applyTo(currentProgram, monitor);
            if (!ok) {
                currentProgram.getFunctionManager().removeFunction(entry);
                new CreateFunctionCmd(entry).applyTo(currentProgram, monitor);
            }
            f = getFunctionAt(entry);
            if (f == null) { out.println("  function vanished at " + entry); return null; }
        }
        out.println("  repaired " + f.getName() + " body " + before + " -> " + f.getBody().getNumAddresses() + " bytes, ranges="
                + f.getBody().getNumAddressRanges() + " max=" + f.getBody().getMaxAddress());
        return f;
    }

    void dumpFn(Function f, boolean disasm) throws Exception {
        out.println("Function: " + f.getName() + " @ " + f.getEntryPoint() + " (raw 0x"
                + Long.toHexString(f.getEntryPoint().getOffset() - 0x100000L) + ") size=" + f.getBody().getNumAddresses()
                + " noreturn=" + f.hasNoReturn());
        out.print("Callees:");
        for (Function c : f.getCalledFunctions(monitor)) out.print(" " + c.getName() + "@" + c.getEntryPoint() + (c.hasNoReturn() ? "(noreturn)" : ""));
        out.println();
        if (decompiled.contains(f.getEntryPoint())) { out.println("  (already decompiled above)"); return; }
        decompiled.add(f.getEntryPoint());
        if (disasm) {
            out.println("--- disassembly ---");
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (it.hasNext()) { Instruction ins = it.next(); out.println("  " + ins.getAddress() + "  " + ins); }
        }
        out.println("--- decompile ---");
        DecompileResults res = decomp.decompileFunction(f, 300, monitor);
        if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
        else out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
        out.println();
    }

    List<Address> findPointers(long value) throws Exception {
        List<Address> hits = new ArrayList<>();
        Memory mem = currentProgram.getMemory();
        byte[] pat = new byte[8];
        for (int i = 0; i < 8; i++) pat[i] = (byte) ((value >>> (8 * i)) & 0xff);
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute()) continue;
            Address a = mem.findBytes(b.getStart(), b.getEnd(), pat, null, true, monitor);
            while (a != null && hits.size() < 40) {
                hits.add(a);
                if (a.add(8).compareTo(b.getEnd()) >= 0) break;
                a = mem.findBytes(a.add(8), b.getEnd(), pat, null, true, monitor);
            }
        }
        return hits;
    }

    void describe(Address a) {
        Function tf = getFunctionAt(a);
        Symbol s = getSymbolAt(a);
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        out.print(" -> " + a + (b != null ? "[" + b.getName() + "]" : "") + (tf != null ? " " + tf.getName() : "") + (s != null ? " sym=" + s.getName() : ""));
    }

    void rttiChase(String s) throws Exception {
        Memory mem = currentProgram.getMemory();
        byte[] pat = (s + "\0").getBytes("ASCII");
        Address str = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
        out.println("=================================================================");
        out.println("=== string \"" + s + "\" at " + str + " ===");
        if (str == null) return;
        ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(str);
        int n = 0;
        while (rit.hasNext()) { Reference r = rit.next(); n++; Function cf = getFunctionContaining(r.getFromAddress());
            out.println("  code/data ref from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (cf != null ? cf.getName() : "<none>")); }
        if (n == 0) out.println("  (no Ghidra references recorded)");
        List<Address> ptrs = findPointers(str.getOffset());
        out.println("  raw pointers to string: " + ptrs.size());
        for (Address p : ptrs) {
            out.print("  ptr at"); describe(p); out.println();
            Address ti = p.subtract(8);
            long vp = mem.getLong(ti);
            out.println("    typeinfo candidate " + ti + " vptr=0x" + Long.toHexString(vp));
            List<Address> vt = findPointers(ti.getOffset());
            for (Address v : vt) {
                out.print("    typeinfo referenced from"); describe(v); out.println();
                Address slot0 = v.add(8);
                out.println("    vtable candidate (slots from " + slot0 + "):");
                for (int i = 0; i < 20; i++) {
                    Address sa = slot0.add(8L * i);
                    long fv = mem.getLong(sa);
                    Address fa = toAddr(fv);
                    MemoryBlock fb = mem.getBlock(fa);
                    if (fb == null || !fb.isExecute()) { if (i > 2) break; out.println("      slot" + i + " 0x" + Long.toHexString(fv) + " (non-code)"); continue; }
                    Function ff = getFunctionAt(fa);
                    out.println("      slot" + i + " " + fa + " " + (ff != null ? ff.getName() + " size=" + ff.getBody().getNumAddresses() : "<no function>"));
                }
            }
        }
    }

    @Override
    public void run() throws Exception {
        out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            hygiene();

            out.println("=================================================================");
            out.println("=== BODY REPAIR ===");
            Map<Long, Function> resolved = new LinkedHashMap<>();
            for (long[] t : TARGETS) {
                Address a = toAddr(t[0]);
                out.println("target " + a + " (raw 0x" + Long.toHexString(t[0] - 0x100000L) + ")");
                Function f = getFunctionContaining(a);
                if (f != null) { f = repair(f.getEntryPoint()); }
                if (f == null || !f.getBody().contains(a)) {
                    Function bf = getFunctionBefore(a);
                    out.println("  not contained; function before: " + (bf != null ? bf.getName() + "@" + bf.getEntryPoint() + " max=" + bf.getBody().getMaxAddress() : "none"));
                    if (bf != null) { Function r = repair(bf.getEntryPoint()); if (r != null && r.getBody().contains(a)) f = r; }
                }
                if ((f == null || !f.getBody().contains(a)) && t[1] != 0) {
                    out.println("  creating at guessed entry " + toAddr(t[1]));
                    Function existing = getFunctionAt(toAddr(t[1]));
                    if (existing == null) {
                        Function cont = getFunctionContaining(toAddr(t[1]));
                        if (cont != null) out.println("  guessed entry lies inside " + cont.getName() + "@" + cont.getEntryPoint());
                    }
                    Function r = repair(toAddr(t[1]));
                    if (r != null) { out.println("  contains target? " + r.getBody().contains(a)); f = r; }
                }
                if (f != null) resolved.put(t[0], f);
            }
            out.println();

            out.println("=================================================================");
            out.println("=== OFFICIAL-MODULE FIXEDSTRING TABLE (ghidra 0x7e5af30.., raw 0x7d5af30..) ===");
            Memory mem = currentProgram.getMemory();
            for (long a = 0x7e5af30L; a <= 0x7e5aff0L; a += 4) {
                Address p = toAddr(a);
                Symbol s = getSymbolAt(p);
                int v = mem.getInt(p);
                ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(p);
                StringBuilder refs = new StringBuilder();
                int n = 0;
                while (rit.hasNext() && n < 8) { Reference r = rit.next(); n++; Function cf = getFunctionContaining(r.getFromAddress());
                    refs.append(" ").append(r.getFromAddress()).append(r.getReferenceType().isWrite() ? "(W" : "(R").append(cf != null ? "," + cf.getName() : "").append(")"); }
                out.println("  " + p + " (raw 0x" + Long.toHexString(a - 0x100000L) + ") file value=0x" + Integer.toHexString(v)
                        + (s != null ? " sym=" + s.getName() : "") + " refs:" + refs);
            }
            out.println();

            out.println("=================================================================");
            out.println("=== DECOMPILES ===");
            for (long[] t : TARGETS) {
                Function f = resolved.get(t[0]);
                out.println("=================================================================");
                out.println("=== target 0x" + Long.toHexString(t[0]) + " (raw 0x" + Long.toHexString(t[0] - 0x100000L) + ") ===");
                if (f == null) { out.println("  <unresolved>"); continue; }
                boolean disasm = t[0] == 0x038675f0L || t[0] == 0x03867580L;
                dumpFn(f, disasm);
            }

            out.println("=================================================================");
            out.println("=== XREFS after repair ===");
            for (long a : new long[]{0x038675f0L, 0x03867580L}) {
                out.println("refs to " + toAddr(a) + ":");
                ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(toAddr(a));
                while (rit.hasNext()) { Reference r = rit.next(); Function cf = getFunctionContaining(r.getFromAddress());
                    out.println("  from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "<none>")); }
            }
            out.println();

            for (String s : STRINGS) rttiChase(s);
        } finally { out.close(); }
        println("Done, wrote " + OUT_PATH);
    }
}
