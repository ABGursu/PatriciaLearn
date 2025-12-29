# Run all benchmark scenarios
$env:Path += ";C:\msys64\mingw64\bin"

$scenarios = @(
    @{Name="Default"; Scenario="default"; Desc="Armageddon=OFF, Psychology=OFF, Fortress=OFF"},
    @{Name="Armageddon Only"; Scenario="armageddon_only"; Desc="Armageddon=ON, Psychology=OFF, Fortress=OFF"},
    @{Name="Armageddon + Psychology"; Scenario="armageddon_psychology"; Desc="Armageddon=ON, Psychology=ON, Fortress=OFF"},
    @{Name="Fortress Only"; Scenario="fortress_only"; Desc="Armageddon=OFF, Psychology=OFF, Fortress=ON"},
    @{Name="Fortress + Psychology"; Scenario="fortress_psychology"; Desc="Armageddon=OFF, Psychology=ON, Fortress=ON"},
    @{Name="Armageddon + Fortress"; Scenario="armageddon_fortress"; Desc="Armageddon=ON, Psychology=OFF, Fortress=ON"},
    @{Name="Armageddon + Psychology + Fortress"; Scenario="armageddon_psychology_fortress"; Desc="Armageddon=ON, Psychology=ON, Fortress=ON"}
)

$results = @()

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  ALL BENCHMARK SCENARIOS (3 Runs)" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

foreach ($scenario in $scenarios) {
    Write-Host "[$($scenarios.IndexOf($scenario) + 1)/$($scenarios.Count)] $($scenario.Name)" -ForegroundColor Yellow
    Write-Host "  $($scenario.Desc)" -ForegroundColor Gray
    
    $output = .\bench_test_multiple.ps1 -Scenario $scenario.Scenario -Runs 3
    $nodes = ($output | Select-String -Pattern "Nodes=(\d+)").Matches.Groups[1].Value
    $nps = ($output | Select-String -Pattern "NPS=(\d+)").Matches.Groups[1].Value
    
    $results += @{
        Name = $scenario.Name
        Desc = $scenario.Desc
        Nodes = [int64]$nodes
        NPS = [int64]$nps
    }
    
    Write-Host ""
}

# Calculate baseline (first scenario)
$baselineNPS = $results[0].NPS

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  BENCHMARK RESULTS SUMMARY" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

Write-Host "| # | Scenario | Nodes | NPS | vs Baseline |" -ForegroundColor White
Write-Host "|---|---------|-------|-----|-----------------|" -ForegroundColor White

for ($i = 0; $i -lt $results.Count; $i++) {
    $result = $results[$i]
    $percentage = [math]::Round(($result.NPS / $baselineNPS) * 100, 1)
    $diff = $result.NPS - $baselineNPS
    $diffPercent = [math]::Round(($diff / $baselineNPS) * 100, 1)
    
    $diffStr = if ($diffPercent -gt 0) { "+$diffPercent%" } else { "$diffPercent%" }
    $color = if ($diffPercent -gt 0) { "Green" } elseif ($diffPercent -lt -10) { "Red" } else { "Yellow" }
    
    Write-Host "| $($i+1) | $($result.Name) | $($result.Nodes) | $($result.NPS) | $diffStr |" -ForegroundColor $color
}

Write-Host "`n========================================" -ForegroundColor Cyan

