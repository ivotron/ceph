  $ cauthtool kring --create-keyring --gen-key
  creating kring

  $ cauthtool --cap osd 'allow rx pool=swimming' kring
  $ cauthtool kring --list|grep caps:
  \tcaps: [osd] allow rx pool=swimming (esc)

# TODO it seems --cap overwrites all previous caps; is this wanted?
  $ cauthtool --cap mds 'allow' kring
  $ cauthtool kring --list|grep caps:
  \tcaps: [mds] allow (esc)
