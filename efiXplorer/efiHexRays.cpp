/*
 * efiXplorer
 * Copyright (C) 2020-2021 Binarly, Rolf Rolles
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * efiHexRays.cpp
 *
 */

#include "efiHexRays.h"

// Given a tinfo_t specifying a user-defined type (UDT), look up the specified
// field by its name, and retrieve its offset.
bool OffsetOf(tinfo_t tif, const char *name, unsigned int *offset) {
    // Get the udt details
    udt_type_data_t udt;
    if (!tif.get_udt_details(&udt)) {
        qstring str;
        tif.get_type_name(&str);
        msg("[E] Could not retrieve udt_type_data_t for %s\n", str.c_str());
        return false;
    }

    // Find the udt member
    udt_member_t udm;
    udm.name = name;
    int fIdx = tif.find_udt_member(&udm, STRMEM_NAME);
    if (fIdx < 0) {
        qstring tstr;
        tif.get_type_name(&tstr);
        msg("[E] Could not find UDT member %s::%s\n", tstr.c_str(), name);
        return false;
    }

    // Get the offset of the field
    *offset = static_cast<unsigned int>(udt.at(fIdx).offset >> 3ULL);
    return true;
}

// Utility function to set a Hex-Rays variable type
bool SetHexRaysVariableType(ea_t funcEa, lvar_t &ll, tinfo_t tif) {
    lvar_saved_info_t lsi;
    lsi.ll = ll;
    lsi.type = tif;
    if (!modify_user_lvar_info(funcEa, MLI_TYPE, lsi)) {
        msg("[E] %a: could not modify lvar type for %s\n", funcEa, ll.name.c_str());
        return false;
    }
    return true;
}

// I added this bit of logic when I noticed that sometimes Hex-Rays will
// aggressively create arrays on the stack. So, I wanted to apply types to
// stack "variables" (whose pointers are passed to the protocol location
// functions), but according to Hex-Rays, they weren't "variables", they
// were arrays. This bit of logic generically detects arrays of either POD
// types, or perhaps pointers to POD types. The final argument allows the
// caller to specify the maximum depth "depth" of the pointers. E.g. at
// depth 1, "int *[10]" is acceptable. At depth 2, "int **[10]" is acceptable.
bool IsPODArray(tinfo_t tif, unsigned int ptrDepth = 0) {
    // If it's not an array, we're done
    if (!tif.is_array())
        return false;

    qstring tstr;

    // If it is an array, we should be able to get its array details.
    array_type_data_t atd;
    if (!tif.get_array_details(&atd)) {
        tif.get_type_name(&tstr);
        msg("[E] %s: can't get array details, despite being an array\n", tstr.c_str());
        return false;
    }

    // Get the element type from the array
    tinfo_t et = atd.elem_type;

    // Start off with depth + 1, so the loop will execute at least once
    int iDepth = ptrDepth + 1;

    // Loop over the user-specified depth
    while (iDepth > 0) {

        // Use typeid last checks. I should clean this up; I'm sure I can get rid
        // of one of them.
        bool b1 = is_typeid_last(et.get_realtype());
        bool b2 = et.is_decl_last();

        // Debug printing
        et.get_type_name(&tstr);
        msg("[I] IsPodArray[%d]: elem_type = %s, b1 = %d, b2 = %d\n", iDepth,
            tstr.c_str(), b1, b2);

        // If it was an integer type, return true
        if (b1 || b2)
            return true;

        // Otherwise, this is where the "pointer depth" comes in.
        // If we haven't exhausted the pointer depth,
        if (--iDepth > 0) {
            // Remove one layer of indirection from the element type
            if (et.is_ptr())
                et = remove_pointer(et);

            // Unless it's not a pointer, then return false.
            else
                return false;
        }
    }

    // If the array wasn't pointers of POD types up to the specified depth, we
    // failed. Return false.
    return false;
}

// Utility function to get a printable qstring from a cexpr_t
const char *Expr2String(cexpr_t *e, qstring *out) {
    e->print1(out, NULL);
    tag_remove(out);
    return out->c_str();
}

void applyAllTypesForInterfaces(std::vector<json> protocols) {
    // Descriptors for EFI_BOOT_SERVICES functions
    struct TargetFunctionPointer BootServicesFunctions[3]{
        {"HandleProtocol", 0x98, 3, 1, 2},
        {"LocateProtocol", 0x140, 3, 0, 2},
        {"OpenProtocol", 0x118, 6, 1, 2}};

    // Descriptors for _EFI_SMM_SYSTEM_TABLE2 functions
    struct TargetFunctionPointer SmmServicesFunctions[2]{
        {"SmmHandleProtocol", 0xb8, 3, 1, 2},
        {"SmmLocateProtocol", 0xd0, 3, 0, 2},
    };

    // Initialize
    ServiceDescriptor sdBs;
    ServiceDescriptor sdSmm;
    sdBs.Initialize("EFI_BOOT_SERVICES", BootServicesFunctions, 3);
    sdSmm.Initialize("_EFI_SMM_SYSTEM_TABLE2", SmmServicesFunctions, 2);

    ServiceDescriptorMap mBs;
    ServiceDescriptorMap mSmm;
    mBs.Register(sdBs);
    mSmm.Register(sdSmm);

    GUIDRetyper retyperBs(mBs);
    GUIDRetyper retyperSmm(mSmm);
    retyperBs.SetProtocols(protocols);
    retyperSmm.SetProtocols(protocols);

    // Handle all protocols
    for (auto protocol : protocols) {
        auto code_addr = protocol["ea"];
        auto service = protocol["service"];

        func_t *f = get_func(code_addr);
        if (f == nullptr) {
            continue;
        }

        retyperBs.SetCodeEa(code_addr);
        retyperSmm.SetCodeEa(code_addr);
        retyperBs.SetFuncEa(f->start_ea);
        retyperSmm.SetFuncEa(f->start_ea);

        hexrays_failure_t hf;
        cfuncptr_t cfunc = decompile(f, &hf);

        // Сheck that the function is decompiled
        if (cfunc == nullptr) {
            continue;
        }

        retyperBs.apply_to(&cfunc->body, nullptr);
        retyperSmm.apply_to(&cfunc->body, nullptr);
    }
}