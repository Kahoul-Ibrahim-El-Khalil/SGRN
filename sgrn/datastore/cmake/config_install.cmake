# cmake/config_install.cmake - Configuration file installation

# Function to install configuration files to the configured directory
function(sgrn_install_config_file config_file)
    # Get the relative path from PROJECT_ROOT/configs
    file(RELATIVE_PATH rel_path "${PROJECT_SOURCE_DIR}/configs" "${config_file}")
    
    # Get the destination directory
    get_filename_component(dest_dir "${SGRN_CONFIG_INSTALL_DIR}/${rel_path}" DIRECTORY)
    
    install(FILES "${config_file}"
        DESTINATION "${dest_dir}"
        COMPONENT config
    )
endfunction()

# Function to install an entire configuration directory
function(sgrn_install_config_directory config_dir)
    # Install all files in the directory, maintaining structure
    install(DIRECTORY "${config_dir}/"
        DESTINATION "${SGRN_CONFIG_INSTALL_DIR}"
        COMPONENT config
        FILES_MATCHING
        PATTERN "*.json"
        PATTERN "*.yaml"
        PATTERN "*.yml"
        PATTERN "*.conf"
        PATTERN "*.cfg"
        PATTERN "*.ini"
        PATTERN "*.toml"
        PATTERN "*.xml"
        PATTERN "*.sh"
        PATTERN "*.service"
        PATTERN "*.conf"
        PATTERN "*.txt" EXCLUDE
        PATTERN "*.md" EXCLUDE
        PATTERN ".git" EXCLUDE
        PATTERN "*.swp" EXCLUDE
        PATTERN "*~" EXCLUDE
    )
endfunction()

# Convenience function to install common config structure
# Usage: sgrn_install_configs()
# This will install ${PROJECT_SOURCE_DIR}/configs/ to ${SGRN_CONFIG_INSTALL_DIR}
function(sgrn_install_configs)
    set(config_source "${PROJECT_SOURCE_DIR}/configs")
    
    if(EXISTS "${config_source}")
        message(STATUS "Installing configurations from ${config_source}")
        message(STATUS "  -> ${SGRN_CONFIG_INSTALL_DIR}")
        sgrn_install_config_directory("${config_source}")
    else()
        message(WARNING "Configuration directory not found: ${config_source}")
    endif()
endfunction()