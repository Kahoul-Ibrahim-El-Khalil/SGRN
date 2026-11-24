package("xml_h")
    set_kind("library", {headeronly = true})
    set_description("xml.h - single-header XML parser")
    set_license("MIT")

    add_urls("https://github.com/mrvladus/xml.h.git")
    add_versions("2.1", "2.1")

    on_install(function (package)
        os.cp("xml.h", package:installdir("include"))
    end)
