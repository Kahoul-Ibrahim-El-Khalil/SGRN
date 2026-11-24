-- AWS SDK (S3 + CRT) from $CONDA_PREFIX, micromamba env, or .prefix.
package("sgrn_aws")
    set_description("AWS C++ SDK (S3), local installation")

    on_fetch(function (package, opt)
        local prefixes = {
            path.join(os.projectdir(), ".prefix", "linux-static"),
            path.join(os.projectdir(), ".prefix", "linux"),
            "/home/odahim/micromamba/envs/SGRN",
            "/usr/local",
            "/usr"
        }
        local conda = os.getenv("CONDA_PREFIX")
        if conda and conda ~= "" then
            table.insert(prefixes, 1, conda)
        end

        local comps = {"aws-cpp-sdk-s3", "aws-cpp-sdk-core", "aws-crt-cpp", "aws-c-event-stream",
                       "aws-c-common", "aws-c-io", "aws-c-mqtt", "aws-c-http", "aws-c-auth",
                       "aws-c-cal", "aws-c-compression", "aws-c-sdkutils", "aws-checksums", "s2n"}

        for _, prefix in ipairs(prefixes) do
            local incdir = path.join(prefix, "include")
            if os.isfile(path.join(incdir, "aws/core/client/AWSError.h")) or
               os.isfile(path.join(incdir, "aws/core/Aws.h")) then
                local candidate_libdirs = {path.join(prefix, "lib"), path.join(prefix, "lib64")}
                for _, libdir in ipairs(candidate_libdirs) do
                    local libs, have_core, have_s3 = {}, false, false
                    for _, c in ipairs(comps) do
                        -- Prefer the unversioned shared object, then the static
                        -- archive. Keep the file path so it can be linked by
                        -- absolute path below.
                        local libfile
                        for _, pattern in ipairs({"lib" .. c .. ".so", "lib" .. c .. ".dylib", "lib" .. c .. ".a"}) do
                            local files = os.files(path.join(libdir, pattern))
                            if #files > 0 then
                                libfile = files[1]
                                break
                            end
                        end
                        if libfile then
                            libs[c] = libfile
                            if c == "aws-cpp-sdk-core" then have_core = true end
                            if c == "aws-cpp-sdk-s3" then have_s3 = true end
                        end
                    end
                    if have_core and have_s3 then
                        -- Link the AWS libraries by absolute path and avoid
                        -- adding the prefix's lib dir to -L. A shared conda
                        -- prefix also ships an older libfmt.so, which would
                        -- shadow the pinned fmt package if it came first in
                        -- the link search path.
                        local ldflags = {"-Wl,--no-as-needed"}
                        for _, c in ipairs(comps) do
                            if libs[c] then
                                table.insert(ldflags, libs[c])
                            end
                        end
                        table.insert(ldflags, "-Wl,-rpath," .. libdir)
                        return {includedirs = {incdir}, ldflags = ldflags}
                    end
                end
            end
        end
    end)
