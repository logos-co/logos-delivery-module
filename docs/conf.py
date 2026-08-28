# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information
import os
import subprocess

project = 'Logos Delivery Module'
copyright = '2026, Institute of Free Technology'
author = 'Institute of Free Technology'

selfpath = os.path.dirname(os.path.abspath(__file__))
git_tag = 'preview'
try:
  git_tag = subprocess.check_output(
    ['git', 'describe', '--tags', '--abbrev=0'],
    cwd=selfpath,
    stderr=subprocess.DEVNULL,
  ).decode().strip().lstrip('v')
except Exception:
  pass

version = git_tag
release = git_tag

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = []
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

root_doc = 'index'

# The @code{.json} blocks in the C++ headers are type schemas (e.g. "id": string),
# not valid JSON, so Pygments cannot highlight them. They still render fine in
# relaxed mode; silence only this category so -W stays meaningful for real issues.
suppress_warnings = ['misc.highlighting_failure']

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'pydata_sphinx_theme'
html_static_path = ['_static']

## -- MyST configuration -----------------------------------------------------
# The guides under docs/ are Markdown, so they read well on GitHub as well as
# here. MyST renders them into the same toctree as the reStructuredText pages.

extensions += ['myst_parser']

# run-node.md links to its own sections (e.g. [With Docker](#with-docker)).
# Anchors are only generated for headings up to this depth.
myst_heading_anchors = 4

## -- Breathe configuration --------------------------------------------------

extensions += ['breathe']

breathe_projects = {
    "Logos Delivery Module":
      os.path.join(selfpath, "xml")
}

breathe_default_project = "Logos Delivery Module"

# -- Theme ------------------------------------------------------------------
html_logo = "_static/logos-logo-dark.png"
html_favicon = "_static/logos-logo-dark.png"
html_css_files = ["custom.css"]

html_theme_options = {
  "external_links": [
    {
      "name": "Tutorial",
      "url": "https://logos-co.github.io/logos-doctest-hub/#logos-delivery-module/ubuntu-latest/running-this-delivery-module-against-logoscore",
    },
    {
      "name": "Logos",
      "url": "https://logos.co",
    }
  ],
  "icon_links": [
    {
      "name": "GitHub",
      "url": "https://github.com/logos-co/logos-delivery-module",
      "icon": "fa-brands fa-github",
    }
  ],
   "logo": {
        "text": f"Delivery Module {version}",
        "image_dark": "_static/logos-logo-dark.png",
    },
   "switcher": {
        "json_url": os.environ.get(
            "SWITCHER_JSON_URL",
            "https://logos-co.github.io/logos-delivery-module/switcher.json"),
        "version_match": version,
    },
   "navbar_end": ["version-switcher", "navbar-icon-links"],
}

# Follow the OS light/dark preference instead of showing a theme toggle.
html_context = {"default_mode": "auto"}
