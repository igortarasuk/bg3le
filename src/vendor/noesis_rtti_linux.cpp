// Weak references for the Noesis C++ RTTI the vendored bg3se code needs.
//
// bg3se reaches Noesis types through typeid and dynamic_cast in its generated
// property-map metadata, which on Windows resolves against the Noesis import
// library. Nothing on Linux can satisfy those symbols:
//
//   - the SDK tools/fetch-externals.sh downloads is headers plus Windows .lib
//     files, so there is no Linux Noesis library to link;
//   - the native game carries no Noesis typeinfo either. It has 28,992 Noesis
//     symbols in .symtab and not one typeinfo, because Noesis uses its own
//     reflection system (Reflection::RegisterType, TypeClass) rather than C++
//     RTTI, so it is almost certainly built with RTTI disabled.
//
// Declaring them weak lets the library link and load with the references
// unresolved, rather than refusing to load. That is sound only because
// nothing here performs RTTI on a Noesis type: the Noesis-backed Ext.ClientUI
// module is not wired up, and the metadata that mentions these types is
// registration data that is never asked to downcast.
//
// If a dynamic_cast on one of these ever does run it will see a null typeinfo
// and misbehave, so this has to be revisited before Ext.ClientUI is enabled.
// The real fix is to keep the Noesis types out of the generated property maps;
// they are produced by make_property_map.py scanning headers, so it is a
// generator-input change rather than a shim.

__asm__(
    ".weak _ZTIN6Noesis10BaseObjectE\n"
    ".weak _ZTIN6Noesis11BaseCommandE\n"
    ".weak _ZTIN6Noesis11RoutedEventE\n"
    ".weak _ZTIN6Noesis12TypeMetaDataE\n"
    ".weak _ZTIN6Noesis12TypePropertyE\n"
    ".weak _ZTIN6Noesis13BaseComponentE\n"
    ".weak _ZTIN6Noesis13UIElementDataE\n"
    ".weak _ZTIN6Noesis14DependencyDataE\n"
    ".weak _ZTIN6Noesis16DependencyObjectE\n"
    ".weak _ZTIN6Noesis16DispatcherObjectE\n"
    ".weak _ZTIN6Noesis16FrameworkElementE\n"
    ".weak _ZTIN6Noesis18DependencyPropertyE\n"
    ".weak _ZTIN6Noesis18LuaDelegateCommandE\n"
    ".weak _ZTIN6Noesis4TypeE\n"
    ".weak _ZTIN6Noesis5PanelE\n"
    ".weak _ZTIN6Noesis6VisualE\n"
    ".weak _ZTIN6Noesis8TypeMetaE\n"
    ".weak _ZTIN6Noesis9TypeClassE\n"
    ".weak _ZTIN6Noesis9UIElementE\n");
