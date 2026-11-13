# Module startup regressions

```sh
python3 -m unittest discover -s tests -v
```

The nine tests execute the actual daemon-start and audio-launch functions with
private files/processes and simulated SELinux-domain responses. They cover
successful readiness, retry budgets, rejected daemon children, label failure,
probe cleanup and UID/GID launcher arguments. No host SELinux setting or Android
service is changed by this harness. Root-framework cold-boot integration needs
separate device validation.
