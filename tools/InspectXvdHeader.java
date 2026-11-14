// SPDX-License-Identifier: BSD-3-Clause
// Headless postScript for a 0x3000-byte XVD prefix imported as DATA:LE:64:default,
// with -noanalysis. Never import the encrypted payload as executable code.
// Field offsets: emoose/xvdtool, LibXboxOne/XVD/XVDStructs.cs (format reference).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

public class InspectXvdHeader extends GhidraScript {
    private static long pages(long bytes) { return bytes / 4096 + (bytes % 4096 == 0 ? 0 : 1); }
    private static long hashPages(long count) {
        long total = 0;
        do { count = count / 170 + (count % 170 == 0 ? 0 : 1); total += count; } while (count > 1);
        return total;
    }
    private void field(StructureDataType header, int offset, DataType type, String name) {
        header.replaceAtOffset(offset, type, type.getLength(), name, null);
    }
    @Override
    public void run() throws Exception {
        if (!currentProgram.getLanguageID().toString().equals("DATA:LE:64:default"))
            throw new IllegalArgumentException("Import only the header prefix using the DATA language");
        if (currentProgram.getMemory().getSize() != 0x3000)
            throw new IllegalArgumentException("Exactly 0x3000 bytes required; do not import a whole XVD");
        if (pages(4096) != 1 || pages(4097) != 2 || hashPages(170) != 1 || hashPages(171) != 3)
            throw new AssertionError("Page arithmetic self-check failed");
        Address base = currentProgram.getMinAddress();
        byte[] bytes = new byte[0x3000];
        currentProgram.getMemory().getBytes(base, bytes);
        if (!new String(bytes, 0x200, 8, StandardCharsets.US_ASCII).equals("msft-xvd"))
            throw new IllegalArgumentException("Invalid XVD magic");
        ByteBuffer data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        long flags = Integer.toUnsignedLong(data.getInt(0x208));
        long drive = data.getLong(0x218);
        long type = Integer.toUnsignedLong(data.getInt(0x280));
        if (data.getInt(0x20c) != 3 || drive <= 0 || type > 1 || (flags & ~0xffL) != 0)
            throw new IllegalArgumentException("Unsupported header version, drive size, type or flags");
        StructureDataType header = new StructureDataType("XVD_Header_Metadata", 0x2a0);
        field(header, 0, new ArrayDataType(CharDataType.dataType, 8, 1), "magic");
        int[] dwords = {8, 12, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x29c};
        String[] names = {"volume_flags", "format_version", "disk_type", "content_type",
            "embedded_xvd_length", "user_data_length", "xvc_data_length", "dynamic_header_length",
            "block_size", "odk_keyslot_id"};
        for (int i = 0; i < dwords.length; ++i) field(header, dwords[i], DWordDataType.dataType, names[i]);
        field(header, 0x10, QWordDataType.dataType, "created_filetime");
        field(header, 0x18, QWordDataType.dataType, "drive_size");
        field(header, 0x1bc, QWordDataType.dataType, "package_version_raw");
        field(header, 0x294, QWordDataType.dataType, "required_system_version_raw");
        field(header, 0x270, ByteDataType.dataType, "mutable_page_count");
        for (int offset : new int[]{0x40, 0x60, 0x14c, 0x16c})
            field(header, offset, new ArrayDataType(ByteDataType.dataType, 32, 1),
                "opaque_hash_or_key_" + Integer.toHexString(offset));
        clearListing(base, base.add(0x2ff));
        createData(base.add(0x200), header);
        long embedded = Integer.toUnsignedLong(data.getInt(0x288));
        long user = Integer.toUnsignedLong(data.getInt(0x28c));
        long xvc = Integer.toUnsignedLong(data.getInt(0x290));
        long bat = Integer.toUnsignedLong(data.getInt(0x294));
        long hashed = Math.addExact(pages(drive), pages(user) + pages(xvc) + pages(bat));
        long hashOffset = 0x3000 + pages(embedded) * 4096 + Byte.toUnsignedLong(bytes[0x470]) * 4096;
        long treePages = (flags & 4) != 0 ? 0 : hashPages(hashed) * ((flags & 16) != 0 ? 2 : 1);
        long userOffset = Math.addExact(hashOffset, Math.multiplyExact(treePages, 4096));
        long batOffset = userOffset + (pages(user) + pages(xvc)) * 4096;
        println("XVD_HEADER name=" + currentProgram.getName() + " prefix_sha256=" + currentProgram.getExecutableSHA256());
        println(String.format("XVD_METADATA type=%d content=0x%x flags=0x%x revision=0x%016x odk_slot=%d encrypted_by_header=%s",
            type, data.getInt(0x284), flags, data.getLong(0x3bc), data.getInt(0x49c), (flags & 2) == 0));
        println(String.format("XVD_LAYOUT embedded_length=0x%x user_length=0x%x xvc_length=0x%x bat_length=0x%x hash_offset=0x%x hash_pages=%d user_offset=0x%x bat_offset=0x%x drive_data_offset=0x%x",
            embedded, user, xvc, bat, hashOffset, treePages, userOffset, batOffset, batOffset + pages(bat) * 4096));
        println("XVD_HEADER_COMPLETE payload_readable=not_tested signature_verified=false hash_tree_verified=false code_disassembled=false");
    }
}
