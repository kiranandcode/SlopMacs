{
  "targets": [
    {
      "target_name": "fn_key",
      "conditions": [
        ["OS=='mac'", {
          "sources": ["fn_key.m"],
          "xcode_settings": {
            "OTHER_LDFLAGS": ["-framework", "AppKit"],
            "CLANG_ENABLE_OBJC_ARC": "YES"
          }
        }, {
          "sources": []
        }]
      ]
    }
  ]
}
