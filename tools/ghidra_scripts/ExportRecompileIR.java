// Read-only export of a module for the translator: the complete instruction listing with p-code, basic blocks,
// cross references, jump-table candidates, imports, exports, relocations and a coverage report, as gzipped JSON
// lines in the export directory. Extends recomp-kit's tools/ExportProgram.java (upstream
// eb32cc42623e0e9e868b9d27423a57d5a5cba56b, MIT; see third_party/recomp-kit/LICENSE and NOTICE).
// No decompilation, no change to the program.
// Arguments: EXPORT_DIR [MODULE_ID SOURCE_FILE SHA256] (default: Sacred.exe from SACRED_ASSETS).
// @category Sacred

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.model.address.*;
import ghidra.program.model.block.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.GZIPOutputStream;

public class ExportRecompileIR extends GhidraScript {
    private static final Path ROOT =
            Path.of(System.getenv().getOrDefault("SACRED_ROOT", System.getProperty("user.dir")));
    private static final Set<Integer> PREFIX_BYTES =
            Set.of(0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x66, 0x67);
    private static final String[] WIDTH_NAMES = {
            "zmmword", "ymmword", "xmmword", "tbyte", "qword", "dword", "word", "byte"};
    private static final int[] WIDTH_BITS = {512, 256, 128, 80, 64, 32, 16, 8};

    private String expectedHash = "db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74";
    private String moduleId = "sacred.exe";
    private Path source = Path.of(System.getenv().getOrDefault("SACRED_ASSETS", "."), "Sacred.exe");

    private final Gson gson = new GsonBuilder().serializeNulls().disableHtmlEscaping().create();
    private final Map<String, Long> counts = new TreeMap<>();
    private final Map<String, Long> userops = new TreeMap<>();
    private final List<Object> warnings = new ArrayList<>();
    private final List<Object> decodeErrors = new ArrayList<>();
    private final AddressSet listingCode = new AddressSet();
    private final AddressSet orphanCode = new AddressSet();
    private final AddressSet executable = new AddressSet();
    private final Map<Long, Long> pointerSlots = new TreeMap<>();
    private final Set<Long> vtableSlots = new HashSet<>();
    private final List<Object> missingEdges = new ArrayList<>();
    private final List<Object> sections = new ArrayList<>();

    private byte[] image;
    private ByteBuffer pe;
    private long base;
    private long imageSize;
    private long entry;
    private int optional;
    private int directory;
    private Path out;
    private Listing listing;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1 && args.length != 4) {
            throw new IllegalArgumentException("Expected export directory [module source sha256]");
        }

        if (args.length == 4) {
            moduleId = args[1];
            source = Path.of(args[2]);
            expectedHash = args[3];
        }

        out = Path.of(args[0]).toAbsolutePath().normalize();
        if (!out.startsWith(ROOT.resolve("analysis/export"))) {
            throw new IOException("Export must stay in analysis/export");
        }

        Files.createDirectories(out);
        // A failed run cannot leave an earlier successful manifest in place.
        Files.deleteIfExists(out.resolve("manifest.json"));
        listing = currentProgram.getListing();
        readPe();
        println("IR_BEGIN sha256=" + expectedHash);

        exportFunctions();
        findPointerSlots();
        exportPeTables();
        exportInstructions();
        exportBlocks();
        exportXrefs();
        exportCoverage();

        println("IR_COUNTS " + gson.toJson(counts));
        println("IR_EXPORT_COMPLETE");
    }

    // ---- Helpers ----

    private Map<String, Object> obj(Object... pairs) {
        Map<String, Object> map = new LinkedHashMap<>();
        for (int i = 0; i < pairs.length; i += 2) {
            map.put((String) pairs[i], pairs[i + 1]);
        }

        return map;
    }

    private void add(String key) {
        counts.merge(key, 1L, Long::sum);
    }

    private String hex(long n) {
        return String.format("0x%08x", n);
    }

    private String bytes(byte[] b) {
        return HexFormat.of().formatHex(b);
    }

    private Object loc(Address a) {
        if (a == null) {
            return null;
        }

        if (a.isMemoryAddress() && a.getOffset() >= base && a.getOffset() < base + imageSize) {
            return obj("module_id", moduleId, "rva", hex(a.getOffset() - base));
        }

        return obj("space", a.getAddressSpace().getName(), "offset", hex(a.getOffset()));
    }

    private Object rva(long n) {
        return obj("module_id", moduleId, "rva", hex(n));
    }

    private Address addr(long va) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(va);
    }

    private List<Object> ranges(AddressSetView set) {
        List<Object> result = new ArrayList<>();
        for (AddressRange r : set.getAddressRanges()) {
            result.add(obj("first", loc(r.getMinAddress()), "last", loc(r.getMaxAddress())));
        }

        return result;
    }

    private BufferedWriter stream(String name) throws IOException {
        var file = new GZIPOutputStream(Files.newOutputStream(out.resolve(name + ".gz")), 65536);
        return new BufferedWriter(new OutputStreamWriter(file, StandardCharsets.UTF_8), 65536);
    }

    private void line(BufferedWriter w, Object value) throws IOException {
        w.write(gson.toJson(value));
        w.newLine();
    }

    private void json(String name, Object value) throws IOException {
        Files.writeString(out.resolve(name), gson.toJson(value) + "\n", StandardCharsets.UTF_8);
    }

    // ---- PE file ----

    private long u32(int off) {
        return Integer.toUnsignedLong(pe.getInt(off));
    }

    private int u16(int off) {
        return Short.toUnsignedInt(pe.getShort(off));
    }

    private int fileOffset(long rva) {
        if (rva >= 0 && rva < u32(optional + 60)) {
            return Math.toIntExact(rva);
        }

        for (Object s : sections) {
            Map<?, ?> m = (Map<?, ?>) s;
            long va = (long) m.get("rva");
            long raw = (long) m.get("raw_size");
            if (rva >= va && rva - va < raw) {
                return Math.toIntExact((long) m.get("raw_offset") + rva - va);
            }
        }

        throw new IllegalArgumentException("RVA has no file bytes: " + hex(rva));
    }

    private String cstr(int offset) {
        int end = offset;
        while (image[end] != 0) {
            end++;
        }

        return new String(image, offset, end - offset, StandardCharsets.ISO_8859_1);
    }

    private void readPe() throws Exception {
        image = Files.readAllBytes(source);
        String hash = bytes(MessageDigest.getInstance("SHA-256").digest(image));
        if (!expectedHash.equals(hash) || !expectedHash.equals(currentProgram.getExecutableSHA256())) {
            throw new IOException("Input/project hash mismatch: " + hash);
        }

        pe = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
        int coff = Math.toIntExact(u32(0x3c)) + 4;
        optional = coff + 20;
        if (pe.getInt(coff - 4) != 0x4550 || u16(coff) != 0x14c || u16(optional) != 0x10b) {
            throw new IOException("Expected PE32 i386");
        }

        base = u32(optional + 28);
        imageSize = u32(optional + 56);
        entry = u32(optional + 16);
        if (currentProgram.getImageBase().getOffset() != base) {
            throw new IOException("Image base mismatch");
        }

        directory = optional + 96;
        int table = optional + u16(coff + 16);
        for (int n = 0; n < u16(coff + 2); n++) {
            int p = table + n * 40;
            String name = new String(image, p, 8, StandardCharsets.ISO_8859_1).split("\0", 2)[0];
            long size = u32(p + 8);
            long va = u32(p + 12);
            long flags = u32(p + 36);
            sections.add(obj("name", name, "rva", va, "virtual_size", size, "raw_size", u32(p + 16), "raw_offset",
                    u32(p + 20), "characteristics", hex(flags), "read", (flags & 0x40000000L) != 0, "write",
                    (flags & 0x80000000L) != 0, "execute", (flags & 0x20000000L) != 0));

            long mappedSize = Math.max(size, u32(p + 16));
            if ((flags & 0x20000000L) != 0 && mappedSize > 0) {
                executable.add(addr(base + va), addr(base + va + mappedSize - 1));
            }
        }
    }

    // Every instruction byte must equal the file's byte at that place (zero-filled section tails excepted).
    private void validate(Instruction i) throws Exception {
        byte[] decoded = i.getBytes();
        long r = i.getAddress().getOffset() - base;
        if (r < 0 || r + decoded.length > imageSize) {
            throw new IOException("Instruction outside PE image: " + i.getAddress());
        }

        for (int n = 0; n < decoded.length; n++) {
            int expected = 0;
            boolean mapped = r + n < u32(optional + 60);
            if (mapped) {
                expected = image[Math.toIntExact(r + n)] & 255;
            } else {
                for (Object s : sections) {
                    Map<?, ?> m = (Map<?, ?>) s;
                    long va = (long) m.get("rva");
                    long raw = (long) m.get("raw_size");
                    long extent = Math.max(raw, (long) m.get("virtual_size"));
                    if (r + n >= va && r + n - va < extent) {
                        mapped = true;
                        if (r + n - va < raw) {
                            expected = image[fileOffset(r + n)] & 255;
                        }

                        break;
                    }
                }
            }

            if (!mapped || expected != (decoded[n] & 255)) {
                throw new IOException("Instruction byte mismatch at " + i.getAddress() + "+" + n);
            }
        }

        if (listingCode.intersects(i.getMinAddress(), i.getMaxAddress())) {
            throw new IOException("Overlapping listing instruction at " + i.getAddress());
        }

        listingCode.add(i.getMinAddress(), i.getMaxAddress());
        counts.merge("validated_instruction_bytes", (long) decoded.length, Long::sum);
    }

    // ---- Instructions ----

    private Object token(Object o) {
        if (o instanceof Register r) {
            return obj("kind", "register", "name", r.getName(), "bits", r.getBitLength());
        }

        if (o instanceof Scalar s) {
            return obj("kind", "scalar", "bits", s.bitLength(), "unsigned", Long.toUnsignedString(s.getUnsignedValue()),
                    "signed", s.getSignedValue());
        }

        if (o instanceof Address a) {
            return obj("kind", "address", "target", loc(a));
        }

        return obj("kind", "syntax", "text", o.toString());
    }

    private Object node(Varnode v) {
        if (v == null) {
            return null;
        }

        Map<String, Object> m = obj("space", v.getAddress().getAddressSpace().getName(), "offset", hex(v.getOffset()),
                "bytes", v.getSize());
        if (v.isAddress()) {
            m.put("location", loc(v.getAddress()));
        }

        if (v.isRegister()) {
            Register r = currentProgram.getRegister(v.getAddress(), v.getSize());
            m.put("register", r == null ? null : r.getName());
        }

        return m;
    }

    private List<Object> operands(Instruction i) {
        List<Object> result = new ArrayList<>();
        for (int n = 0; n < i.getNumOperands(); n++) {
            String text = i.getDefaultOperandRepresentation(n);
            List<Object> objects = new ArrayList<>();
            List<Object> expression = new ArrayList<>();
            for (Object o : i.getOpObjects(n)) {
                objects.add(token(o));
            }

            var representation = i.getDefaultOperandRepresentationList(n);
            if (representation != null) {
                for (Object o : representation) {
                    expression.add(token(o));
                }
            }

            Integer width = null;
            String lower = text.toLowerCase(Locale.ROOT);
            for (int k = 0; k < WIDTH_NAMES.length; k++) {
                if (lower.startsWith(WIDTH_NAMES[k] + " ptr")) {
                    width = WIDTH_BITS[k];
                    break;
                }
            }

            Register register = i.getRegister(n);
            if (width == null && register != null) {
                width = register.getBitLength();
            }

            Scalar scalar = i.getScalar(n);
            if (width == null && scalar != null) {
                width = scalar.bitLength();
            }

            if (width == null && i.getAddress(n) != null) {
                width = i.getAddress(n).getAddressSpace().getSize();
            }

            String segment = null;
            for (String s : List.of("CS", "DS", "ES", "FS", "GS", "SS")) {
                if (text.toUpperCase(Locale.ROOT).contains(s + ":")) {
                    segment = s;
                }
            }

            Object effectiveAddress = null;
            if (text.contains("[")) {
                effectiveAddress = obj("segment_override", segment, "expression_tokens", expression, "semantics",
                        "raw p-code LOAD/STORE address expressions");
            }

            result.add(obj("index", n, "type_flags", i.getOperandType(n), "text", text, "width_bits", width,
                    "width_evidence", width == null ? "unresolved" : "Ghidra operand", "objects", objects,
                    "effective_address", effectiveAddress));
        }

        return result;
    }

    private List<Object> prefixes(byte[] b) {
        List<Object> p = new ArrayList<>();
        for (byte v : b) {
            int n = v & 255;
            if (!PREFIX_BYTES.contains(n)) {
                break;
            }

            p.add(String.format("%02x", n));
        }

        return p;
    }

    // Appends to pcode as it goes, so a decode error keeps the operations before it.
    private void pcode(Instruction i, List<Object> pcode) {
        int index = 0;
        for (PcodeOp p : i.getPcode()) {
            List<Object> inputs = new ArrayList<>();
            for (Varnode input : p.getInputs()) {
                inputs.add(node(input));
            }

            String userop = null;
            if (p.getOpcode() == PcodeOp.CALLOTHER) {
                userop = currentProgram.getLanguage().getUserDefinedOpName((int) p.getInput(0).getOffset());
            }

            if (userop != null) {
                userops.merge(userop, 1L, Long::sum);
            }

            AddressSpace memorySpace = null;
            if (p.getOpcode() == PcodeOp.LOAD || p.getOpcode() == PcodeOp.STORE) {
                memorySpace = currentProgram.getAddressFactory().getAddressSpace((int) p.getInput(0).getOffset());
            }

            pcode.add(obj("index", index++, "opcode", p.getMnemonic(), "output", node(p.getOutput()), "inputs", inputs,
                    "userop", userop, "memory_space", memorySpace == null ? null : memorySpace.getName()));
        }
    }

    private void exportInstructions() throws Exception {
        try (var w = stream("instructions.jsonl"); var jumps = stream("jump_tables.jsonl")) {
            for (Instruction i : listing.getInstructions(true)) {
                monitor.checkCancelled();
                validate(i);
                add("instructions");

                boolean orphan = currentProgram.getFunctionManager().getFunctionContaining(i.getAddress()) == null;
                if (orphan) {
                    add("out_of_function_instructions");
                    orphanCode.add(i.getMinAddress(), i.getMaxAddress());
                }

                FlowType flow = i.getFlowType();
                if (flow.isComputed() && flow.isCall()) {
                    add("computed_calls");
                }

                if (flow.isComputed() && flow.isJump()) {
                    add("computed_jumps");
                    line(jumps, jumpTable(i));
                }

                List<Object> pcode = new ArrayList<>();
                String pcodeError = null;
                try {
                    pcode(i, pcode);
                } catch (RuntimeException ex) {
                    pcodeError = ex.toString();
                    add("pcode_decode_errors");
                    decodeErrors.add(obj("address", loc(i.getAddress()), "message", pcodeError));
                }

                List<Object> flows = new ArrayList<>();
                for (Address target : i.getFlows()) {
                    flows.add(loc(target));
                    if (target.isMemoryAddress() && listing.getInstructionAt(target) == null) {
                        missingEdges.add(obj("source", loc(i.getAddress()), "target", loc(target), "reason",
                                "target is not an existing listing instruction boundary"));
                    }
                }

                List<Object> targets = new ArrayList<>();
                Address fallThrough = i.getFallThrough();
                if (flow.isCall() && fallThrough != null) {
                    targets.add(obj("kind", "call_continuation", "target", loc(fallThrough), "boundary_exists",
                            listing.getInstructionAt(fallThrough) != null));
                }

                if (fallThrough != null && listing.getInstructionAt(fallThrough) == null) {
                    missingEdges.add(obj("source", loc(i.getAddress()), "target", loc(fallThrough), "reason",
                            flow.isCall() ? "call continuation lacks a listing boundary"
                                          : "fallthrough lacks a listing boundary"));
                }

                line(w,
                        obj("address", loc(i.getAddress()), "bytes", bytes(i.getBytes()), "length", i.getLength(),
                                "parsed_length", i.getParsedLength(), "prefixes", prefixes(i.getBytes()), "mnemonic",
                                i.getMnemonicString(), "operands", operands(i), "fallthrough", loc(fallThrough),
                                "flow_type", flow.toString(), "flow_override", i.getFlowOverride().toString(), "flows",
                                flows, "targets", targets, "pcode", pcode, "pcode_error", pcodeError,
                                "outside_functions", orphan, "admission",
                                "byte_verified_listing_candidate_not_semantically_trusted"));

                if (counts.get("instructions") % 100000 == 0) {
                    println("IR instructions=" + counts.get("instructions"));
                }
            }
        }
    }

    // ---- Functions, pointers, jump tables ----

    private void exportFunctions() throws Exception {
        try (var w = stream("functions.jsonl")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                add("functions");
                Function thunk = f.isThunk() ? f.getThunkedFunction(true) : null;
                if (f.isThunk()) {
                    add("thunks");
                }

                var callingConvention = obj("name", f.getCallingConventionName(), "source",
                        f.getSignatureSource().toString(), "confidence",
                        f.getSignatureSource() == SourceType.USER_DEFINED ? "user_declared" : "unverified_analysis",
                        "stack_purge_bytes", f.getStackPurgeSize(), "signature", f.getSignature().toString());
                line(w,
                        obj("entry", loc(f.getEntryPoint()), "name", f.getName(), "body", ranges(f.getBody()),
                                "thunk_target", thunk == null ? null : loc(thunk.getEntryPoint()), "calling_convention",
                                callingConvention, "no_return_evidence", f.hasNoReturn(), "admission",
                                "discovery_not_boundary_proof"));
            }
        }
    }

    // Data words that hold the address of an instruction; two or more in a row are vtable candidates.
    private void findPointerSlots() throws Exception {
        var references = currentProgram.getReferenceManager();
        var sources = references.getReferenceSourceIterator(currentProgram.getMemory(), true);
        while (sources.hasNext()) {
            monitor.checkCancelled();
            Address from = sources.next();
            if (!from.isMemoryAddress() || listing.getInstructionContaining(from) != null) {
                continue;
            }

            for (Reference r : references.getReferencesFrom(from)) {
                Address to = r.getToAddress();
                if (!to.isMemoryAddress() || listing.getInstructionAt(to) == null) {
                    continue;
                }

                try {
                    long value = u32(fileOffset(from.getOffset() - base));
                    if (value == to.getOffset()) {
                        pointerSlots.put(from.getOffset(), value);
                    }
                } catch (IllegalArgumentException ex) {
                    // Non-file-backed/synthetic data is not an original image pointer.
                }
            }
        }

        for (long slot : pointerSlots.keySet()) {
            if (pointerSlots.containsKey(slot - 4) || pointerSlots.containsKey(slot + 4)) {
                vtableSlots.add(slot);
            }
        }
    }

    private Object jumpTable(Instruction i) {
        List<Object> destinations = new ArrayList<>();
        List<Object> storage = new ArrayList<>();
        for (Reference r : i.getReferencesFrom()) {
            if (r.getReferenceType().isFlow() && r.getToAddress().isMemoryAddress()) {
                destinations.add(loc(r.getToAddress()));
            }
        }

        for (int n = 0; n < i.getNumOperands(); n++) {
            for (Object o : i.getOpObjects(n)) {
                long candidate = -1;
                if (o instanceof Scalar s) {
                    candidate = s.getUnsignedValue();
                } else if (o instanceof Address a) {
                    candidate = a.getOffset();
                }

                if (!pointerSlots.containsKey(candidate)) {
                    continue;
                }

                long end = candidate;
                List<Object> values = new ArrayList<>();
                while (pointerSlots.containsKey(end)) {
                    values.add(loc(addr(pointerSlots.get(end))));
                    end += 4;
                }

                storage.add(obj("first", loc(addr(candidate)), "last", loc(addr(end - 1)), "element_bytes", 4,
                        "encoding", "little_endian_absolute_VA32", "destinations", values, "provenance",
                        "operand constant plus byte-verified data xrefs", "extent_confidence",
                        "candidate_contiguous_pointer_run_not_verified_bounds"));
            }
        }

        if (destinations.isEmpty()) {
            add("computed_jumps_without_memory_flow_refs");
        }

        if (destinations.size() > 1) {
            add("computed_jumps_multiple_memory_flow_refs");
        }

        return obj("dispatch", loc(i.getAddress()), "status", "unresolved", "storage_candidates", storage, "selector",
                null, "default", null, "destinations", destinations, "provenance",
                "existing Ghidra memory flow references", "unresolved",
                List.of("selector/default and complete table bounds are not proven",
                        "recorded flow destinations are recovery evidence, not an exhaustive runtime target set"));
    }

    // ---- Blocks and cross references ----

    private void exportBlocks() throws Exception {
        try (var w = stream("blocks.jsonl")) {
            var blocks = new BasicBlockModel(currentProgram).getCodeBlocks(monitor);
            while (blocks.hasNext()) {
                monitor.checkCancelled();
                CodeBlock b = blocks.next();
                add("blocks");

                List<Object> entries = new ArrayList<>();
                for (Address a : b.getStartAddresses()) {
                    entries.add(loc(a));
                }

                List<Object> members = new ArrayList<>();
                for (Instruction i : listing.getInstructions(b, true)) {
                    members.add(hex(i.getAddress().getOffset() - base));
                }

                List<Object> successors = new ArrayList<>();
                var destinations = b.getDestinations(monitor);
                while (destinations.hasNext()) {
                    var d = destinations.next();
                    successors.add(obj("source", loc(d.getReferent()), "target", loc(d.getReference()), "block_entry",
                            loc(d.getDestinationAddress()), "kind", d.getFlowType().toString()));
                }

                line(w,
                        obj("module_id", moduleId, "entries", entries, "ranges", ranges(b), "instruction_rvas", members,
                                "successors", successors, "landing_pad_policy",
                                "all blocks retained; exception identity remains unverified"));
            }
        }
    }

    private void exportXrefs() throws Exception {
        try (var w = stream("xrefs.jsonl")) {
            var references = currentProgram.getReferenceManager();
            var sources = references.getReferenceSourceIterator(currentProgram.getMemory(), true);
            while (sources.hasNext()) {
                monitor.checkCancelled();
                Address from = sources.next();
                for (Reference r : references.getReferencesFrom(from)) {
                    add("xrefs");
                    Address to = r.getToAddress();
                    boolean flow = r.getReferenceType().isFlow();
                    boolean taken = !flow && to.isMemoryAddress() && listing.getInstructionAt(to) != null;
                    String kind = r.isExternalReference() ? "external" : flow ? "code" : "data";
                    line(w,
                            obj("source", loc(from), "destination", loc(to), "kind", kind, "reference_type",
                                    r.getReferenceType().toString(), "operand_index", r.getOperandIndex(), "provenance",
                                    r.getSource().toString(), "address_taken_entry_candidate", taken,
                                    "candidate_pointer_slot", pointerSlots.containsKey(from.getOffset()),
                                    "vtable_candidate_slot", vtableSlots.contains(from.getOffset()),
                                    "reference_to_vtable_candidate",
                                    to.isMemoryAddress() && vtableSlots.contains(to.getOffset()),
                                    "exception_target_policy", "retain all references without inferring SEH identity"));
                }
            }
        }
    }

    // ---- Imports, exports, relocations ----

    private void exportPeTables() throws Exception {
        json("imports.json", imports());
        json("exports.json", exports());

        long relocRva = u32(directory + 40);
        long relocSize = u32(directory + 44);
        json("relocations.json",
                obj("stripped", (u16(Math.toIntExact(u32(0x3c)) + 22) & 1) != 0, "directory_rva", hex(relocRva),
                        "directory_size", relocSize, "entries", relocations(relocRva, relocSize)));

        for (Symbol s : currentProgram.getSymbolTable().getExternalSymbols()) {
            add("external_symbols");
        }
    }

    private List<Object> imports() throws Exception {
        List<Object> imports = new ArrayList<>();
        long importRva = u32(directory + 8);
        if (importRva == 0) {
            return imports;
        }

        for (int d = fileOffset(importRva); u32(d + 12) != 0; d += 20) {
            String dll = cstr(fileOffset(u32(d + 12)));
            long lookup = u32(d) == 0 ? u32(d + 16) : u32(d);
            long iat = u32(d + 16);
            for (int n = 0;; n++) {
                long value = u32(fileOffset(lookup + 4L * n));
                if (value == 0) {
                    break;
                }

                Long ordinal = (value & 0x80000000L) != 0 ? value & 0xffff : null;
                String name = ordinal == null ? cstr(fileOffset(value) + 2) : null;
                Address slot = addr(base + iat + n * 4L);
                List<Object> uses = new ArrayList<>();
                boolean call = false;
                boolean data = false;
                var references = currentProgram.getReferenceManager().getReferencesTo(slot);
                while (references.hasNext()) {
                    Reference r = references.next();
                    Instruction i = listing.getInstructionContaining(r.getFromAddress());
                    boolean isCall = i != null && i.getFlowType().isCall();
                    call |= isCall;
                    data |= !isCall;
                    uses.add(obj("source", loc(r.getFromAddress()), "kind", isCall ? "call_site" : "data_reference",
                            "reference_type", r.getReferenceType().toString()));
                }

                imports.add(obj("dll", dll, "name", name, "ordinal", ordinal, "iat_slot", loc(slot), "lookup_rva",
                        hex(lookup + n * 4L), "call_use", call, "data_use", data, "uses", uses, "calling_convention",
                        null, "abi_confidence", "not_inferred_from_name"));
                add("imports");
            }
        }

        return imports;
    }

    private List<Object> exports() {
        List<Object> exports = new ArrayList<>();
        long exportRva = u32(directory);
        long exportSize = u32(directory + 4);
        if (exportRva == 0) {
            return exports;
        }

        int d = fileOffset(exportRva);
        Map<Long, List<String>> names = new HashMap<>();
        for (int n = 0; n < u32(d + 24); n++) {
            long index = u16(fileOffset(u32(d + 36) + n * 2L));
            String name = cstr(fileOffset(u32(fileOffset(u32(d + 32) + n * 4L))));
            names.computeIfAbsent(index, k -> new ArrayList<>()).add(name);
        }

        for (int n = 0; n < u32(d + 20); n++) {
            long target = u32(fileOffset(u32(d + 28) + n * 4L));
            if (target == 0) {
                continue;
            }

            boolean forwarded = target >= exportRva && target < exportRva + exportSize;
            exports.add(obj("ordinal", u32(d + 16) + n, "names", names.getOrDefault((long) n, List.of()), "target",
                    rva(target), "forwarder", forwarded ? cstr(fileOffset(target)) : null));
        }

        return exports;
    }

    private List<Object> relocations(long relocRva, long relocSize) throws IOException {
        List<Object> relocs = new ArrayList<>();
        if (relocRva == 0) {
            return relocs;
        }

        for (long at = relocRva; at < relocRva + relocSize;) {
            int d = fileOffset(at);
            long page = u32(d);
            long size = u32(d + 4);
            if (size < 8 || at + size > relocRva + relocSize) {
                throw new IOException("Invalid relocation block");
            }

            for (int n = 8; n < size; n += 2) {
                int value = u16(d + n);
                relocs.add(obj("site", rva(page + (value & 0xfff)), "type", value >>> 12, "provenance",
                        "PE base relocation directory"));
            }

            at += size;
        }

        return relocs;
    }

    // ---- Coverage and manifest ----

    private void exportCoverage() throws Exception {
        List<Object> corrections = new ArrayList<>();
        Path entryTargets = ROOT.resolve("data/entry-targets.json");
        if (Files.exists(entryTargets)) {
            for (var item : JsonParser.parseString(Files.readString(entryTargets)).getAsJsonArray()) {
                if (item.getAsJsonObject().get("module_id").getAsString().equals(moduleId)) {
                    corrections.add(item);
                }
            }
        }

        List<Object> bookmarks = new ArrayList<>();
        var iterator = currentProgram.getBookmarkManager().getBookmarksIterator();
        while (iterator.hasNext()) {
            Bookmark b = iterator.next();
            if (b.getTypeString().equals("Error") || b.getTypeString().equals("Warning")) {
                bookmarks.add(obj("address", loc(b.getAddress()), "type", b.getTypeString(), "category",
                        b.getCategory(), "message", b.getComment()));
            }
        }

        List<Object> spaces = new ArrayList<>();
        for (AddressSpace s : currentProgram.getAddressFactory().getAllAddressSpaces()) {
            spaces.add(obj("name", s.getName(), "id", s.getSpaceID(), "bits", s.getSize(), "type", s.getType(),
                    "addressable_unit_bytes", s.getAddressableUnitSize()));
        }

        List<Object> memory = new ArrayList<>();
        for (var b : currentProgram.getMemory().getBlocks()) {
            boolean synthetic = sections.stream().noneMatch(s -> ((Map<?, ?>) s).get("name").equals(b.getName()));
            memory.add(obj("name", b.getName(), "first", loc(b.getStart()), "size", b.getSize(), "read", b.isRead(),
                    "write", b.isWrite(), "execute", b.isExecute(), "initialized", b.isInitialized(),
                    "synthetic_not_PE_section", synthetic));
        }

        List<Object> entries = new ArrayList<>();
        var externalEntries = currentProgram.getSymbolTable().getExternalEntryPointIterator();
        while (externalEntries.hasNext()) {
            entries.add(loc(externalEntries.next()));
        }

        counts.put("historical_warnings", (long) warnings.size());
        counts.put("error_warning_bookmarks", (long) bookmarks.size());
        counts.putIfAbsent("pcode_decode_errors", 0L);

        json("coverage.json",
                obj("counts", counts, "trusted_code", List.of(), "trust_policy",
                        "Byte identity is validated; existing analysis is not semantic or reachability proof.",
                        "admitted_listing_ranges", ranges(listingCode), "uncertain_decode_data",
                        obj("all_listing_instructions", true, "outside_function_ranges", ranges(orphanCode),
                                "bookmarks", bookmarks),
                        "uncovered_executable_ranges", ranges(executable.subtract(listingCode)),
                        "unsupported_instruction_forms",
                        obj("status", "not_qualified_by_the_exporter", "scope",
                                "instruction forms are qualified by the translator, not by this export"),
                        "userops", userops, "unsupported_userops", new ArrayList<>(userops.keySet()),
                        "missing_flow_edges", missingEdges, "computed_target_policy",
                        "all computed sites unresolved pending evidence", "manual_corrections", corrections,
                        "source_warnings", warnings, "pcode_decode_errors", decodeErrors, "required_warning",
                        moduleId.equals("sacred.exe") ? obj("va", "0x00453834", "status",
                                                                "historical_constructor_warning_retained_not_corrected")
                                                      : null));

        byte[] exporter = Files.readAllBytes(ROOT.resolve("tools/ghidra_scripts/ExportRecompileIR.java"));
        json("manifest.json",
                obj("schema", "sacred-recompile-ir", "schema_version", 1, "module_id", moduleId, "sha256", expectedHash,
                        "source_file_bytes", image.length, "machine", "IMAGE_FILE_MACHINE_I386", "image_base",
                        hex(base), "image_size", imageSize, "entry", rva(entry), "ghidra_version",
                        Application.getApplicationVersion(), "exporter_version", 1, "exporter_sha256",
                        bytes(MessageDigest.getInstance("SHA-256").digest(exporter)), "language_version",
                        currentProgram.getLanguage().getVersion(), "language_minor_version",
                        currentProgram.getLanguage().getMinorVersion(), "language",
                        currentProgram.getLanguageID().toString(), "compiler",
                        currentProgram.getCompilerSpec().getCompilerSpecID().toString(), "address_spaces", spaces,
                        "sections", sections, "ghidra_memory_blocks", memory, "external_entry_points", entries,
                        "source_warnings", warnings, "read_only", true, "validation",
                        obj("sha256_matches_project_and_original", true, "all_instruction_bytes_match_PE", true,
                                "contradictory_overlaps", 0, "counts", counts),
                        "jsonl_compression", "gzip", "address_ranges",
                        "inclusive; module references are (module_id,RVA)", "entry_continuation_SEH_policy",
                        "external entries, call continuations, all blocks and all xrefs retained; no guessed SEH "
                                + "labels"));
    }
}
