#!/bin/bash

if [ ! -f ../../sdkconfig ]; then
  echo "Error: No sdkconfig. Cannot determine whether to gzip and minify."
  echo "Please run 'idf.py menuconfig' from the project root directory."
  exit 1
fi


# Clean previous build output and clear working folder
rm -rf dist
rm -rf tmp

# Create build and working directories
mkdir dist
mkdir dist/prov
mkdir tmp

cd tmp

# Convert protocol buffer definitions to JavaScript
pbf ../src/prov/proto/session.proto > session.js
pbf ../src/prov/proto/wifi_scan.proto > wifi_scan.js
pbf ../src/prov/proto/wifi_config.proto > wifi_config.js

# Bundle protocol buffer definitions together with main JavaScript file.
# Browserify handles require() statements in a manner compatible with browsers.
# The protocol definition files created above are no longer needed after this.
cp ../src/prov/prov.js .
browserify prov.js -o prov_bundle_tmp.js

# CSS framework is already minified. Copy as is.
cp ../node_modules/spectre.css/dist/spectre.min.css ../dist/prov/spectre.min.css

# Copy example device hompage and web icon to build output.
cp ../src/index.html ../dist/index.html
cp ../src/favicon.ico ../dist/favicon.ico


# Determine from sdkconfig whether this is a minified and gzipped build.
# Note: This will result in either "y" or ""
export IS_ZIPPED=$(grep ^CONFIG_EXAMPLE_MINIFY_AND_GZIP_WEBPAGES ../../../sdkconfig | sed s/CONFIG_EXAMPLE_MINIFY_AND_GZIP_WEBPAGES=//g)

# If yes
if [ "$IS_ZIPPED" = "y" ]; then
  # Copy index.html for provisioning page, but change "prov_bundle.js" to "prov_bundle.min.js"
  sed s/prov_bundle\.js/prov_bundle\.min\.js/g ../src/prov/index.html > ../dist/prov/index.html

  # Minify JavaScript and HTML while copying to build directory
  uglifyjs prov_bundle_tmp.js > ../dist/prov/prov_bundle.min.js

  # Zip files in place in build directory
  gzip ../dist/prov/index.html
  gzip ../dist/prov/prov_bundle.min.js
  gzip ../dist/prov/spectre.min.css
  gzip ../dist/index.html
else
  # Copy the JavaScript and HTML files as they are to the build output
  cp ../src/prov/index.html ../dist/prov/index.html
  cp prov_bundle_tmp.js ../dist/prov/prov_bundle.js
fi

cd ..
