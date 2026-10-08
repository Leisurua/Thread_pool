param([Parameter(Mandatory)][string]$ResultsDirectory)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$metadata = Get-Content (Join-Path $ResultsDirectory 'environment.json') -Raw | ConvertFrom-Json
if (-not $metadata.completed_at) { throw 'Incomplete benchmark run' }
$metadata.started_at = ([DateTimeOffset]$metadata.started_at).ToOffset([TimeSpan]::FromHours(8)).ToString('o')
$metadata.completed_at = ([DateTimeOffset]$metadata.completed_at).ToOffset([TimeSpan]::FromHours(8)).ToString('o')
$all = @(Import-Csv -LiteralPath (Join-Path $ResultsDirectory 'samples.csv'))
$measured = @($all | Where-Object phase -eq 'measured')
$groups = @($measured | Group-Object suite, implementation, workers, tasks, busy, iterations)
if ($groups.Count -ne 42 -or $measured.Count -ne 42 * $metadata.rounds -or $all.Count -ne 42 * ($metadata.rounds + 1)) {
    throw 'Missing or extra configurations/samples'
}
function Quantile([double[]]$values, [double]$q) {
    $sorted = @($values | Sort-Object)
    $index = ($sorted.Count - 1) * $q
    $lower = [int][Math]::Floor($index)
    $upper = [int][Math]::Ceiling($index)
    return $sorted[$lower] + ($sorted[$upper] - $sorted[$lower]) * ($index - $lower)
}
function Stats([double[]]$values) {
    if ($values.Count -lt 10) { throw 'At least 10 valid values required for every statistic' }
    $mean = ($values | Measure-Object -Average).Average
    $squares = 0.0
    foreach ($value in $values) { $squares += ($value - $mean) * ($value - $mean) }
    return [pscustomobject]@{
        n = $values.Count
        median = Quantile $values 0.5
        min = ($values | Measure-Object -Minimum).Minimum
        max = ($values | Measure-Object -Maximum).Maximum
        p25 = Quantile $values 0.25
        p75 = Quantile $values 0.75
        cv_percent = if ($mean -ne 0) { [Math]::Sqrt($squares / $values.Count) / $mean * 100 } else { 0 }
    }
}
$summaries = [Collections.Generic.List[object]]::new()
foreach ($group in $groups) {
    $first = $group.Group[0]
    if ($group.Count -ne $metadata.rounds -or @($group.Group.round | Sort-Object -Unique).Count -ne $metadata.rounds) { throw 'Incorrect repetitions' }
    if (@($group.Group.checksum | Sort-Object -Unique).Count -ne 1) { throw 'Checksum differs between rounds' }
    [string[]]$metrics = if ($first.suite -like 'close_*') { @('close_ms') } else { @('total_ms') }
    if ($first.implementation -eq 'pool') { $metrics += @('submit_ms', 'collect_ms') }
    if ($first.suite -eq 'submit_100k' -and $first.implementation -eq 'pool') { $metrics += 'avg_submit_us' }
    if ($first.suite -like 'throughput_*') { $metrics += 'tasks_per_second' }
    foreach ($metric in $metrics) {
        [double[]]$values = foreach ($row in $group.Group) {
            switch ($metric) {
                'total_ms' { [double]$row.total_ns / 1e6 }
                'submit_ms' { [double]$row.submit_ns / 1e6 }
                'collect_ms' { [double]$row.collect_ns / 1e6 }
                'close_ms' { [double]$row.close_ns / 1e6 }
                'avg_submit_us' { [double]$row.submit_ns / [double]$row.tasks / 1000 }
                'tasks_per_second' { [double]$row.tasks * 1e9 / [double]$row.total_ns }
            }
        }
        $stats = Stats $values
        $summaries.Add([pscustomobject]@{
            suite = $first.suite; implementation = $first.implementation; workers = [int]$first.workers
            tasks = [int]$first.tasks; busy = [int]$first.busy; iterations = [uint64]$first.iterations
            metric = $metric; n = $stats.n; median = $stats.median; min = $stats.min; max = $stats.max
            p25 = $stats.p25; p75 = $stats.p75; cv_percent = $stats.cv_percent; speedup_vs_serial = $null
        })
    }
}
foreach ($suite in @('submit_100k', 'throughput_10k', 'throughput_100k', 'cpu')) {
    $baseline = @($summaries | Where-Object { $_.suite -eq $suite -and $_.implementation -eq 'serial' -and $_.metric -eq 'total_ms' })[0]
    foreach ($row in @($summaries | Where-Object { $_.suite -eq $suite -and $_.metric -eq 'total_ms' })) {
        $row.speedup_vs_serial = $baseline.median / $row.median
    }
    $checksums = @($measured | Where-Object suite -eq $suite | Select-Object -ExpandProperty checksum -Unique)
    if ($checksums.Count -ne 1) { throw 'Serial/pool workload mismatch' }
}
if ($summaries.Count -ne 88) { throw 'Incorrect number of summary metrics' }
$summaries | Sort-Object suite, implementation, workers, metric | Export-Csv -LiteralPath (Join-Path $ResultsDirectory 'summary.csv') -NoTypeInformation -Encoding utf8
function Number([double]$value, [int]$digits = 3) { return $value.ToString("F$digits", $culture) }
function Cell($row, [int]$digits = 3) {
    if (-not $row) { return 'N/A' }
    return "$(Number $row.median $digits) [$((Number $row.min $digits)), $((Number $row.max $digits))]"
}
function Lookup([string]$suite, [string]$mode, [int]$workers, [string]$metric) {
    return @($summaries | Where-Object { $_.suite -eq $suite -and $_.implementation -eq $mode -and $_.workers -eq $workers -and $_.metric -eq $metric })[0]
}
$report = [Collections.Generic.List[string]]::new()
$report.Add('# Release 性能实测')
$report.Add('')
$report.Add("时间：$($metadata.started_at) 至 $($metadata.completed_at)。CPU：$($metadata.cpu_name)，$($metadata.logical_processors) 个逻辑处理器；Windows $($metadata.os_display_version)，build $($metadata.os_build).$($metadata.os_ubr)。")
$report.Add('')
$report.Add("42 组配置，每组 1 次预热和 $($metadata.rounds) 次实测；共 $($measured.Count) 条实测记录，另有 42 条预热。结果均通过校验。表格为中位数 [最小值, 最大值]；四分位数及变异系数见 [summary.csv](summary.csv)，每次数据见 [samples.csv](samples.csv)，环境、源码/二进制 SHA256 和实际编译命令见 [environment.json](environment.json)。")
$report.Add('')
$report.Add('## 提交开销：10 万极短任务')
$report.Add('')
$report.Add('| worker | 平均 submit 微秒/任务 | submit 阶段 ms | 收集阶段 ms | 整批 ms |')
$report.Add('| --- | --- | --- | --- | --- |')
foreach ($workers in @(1, 2, 4, 8)) {
    $report.Add("| $workers | $(Cell (Lookup submit_100k pool $workers avg_submit_us)) | $(Cell (Lookup submit_100k pool $workers submit_ms)) | $(Cell (Lookup submit_100k pool $workers collect_ms)) | $(Cell (Lookup submit_100k pool $workers total_ms)) |")
}
$report.Add('')
$report.Add("串行直接执行相同极短计算的整批时间：$(Cell (Lookup submit_100k serial 0 total_ms)) ms；串行没有 submit，不能称为提交开销。submit 阶段含任务/future 准备、队列分配、锁、通知及保存 future 句柄，不含 future.get；worker 同时消费队列。")
foreach ($suite in @('throughput_10k', 'throughput_100k')) {
    $report.Add('')
    $report.Add("## 轻量任务吞吐：$suite")
    $report.Add('')
    $report.Add('| 执行方式 | 整批 ms | tasks/s | 相对串行速度 |')
    $report.Add('| --- | --- | --- | --- |')
    foreach ($workers in @(0, 1, 2, 4, 8)) {
        $mode = if ($workers -eq 0) { 'serial' } else { 'pool' }
        $label = if ($workers -eq 0) { '串行' } else { "$workers worker" }
        $time = Lookup $suite $mode $workers total_ms
        $report.Add("| $label | $(Cell $time) | $(Cell (Lookup $suite $mode $workers tasks_per_second) 0) | $(Number $time.speedup_vs_serial)x |")
    }
}
$report.Add('')
$report.Add('## CPU 密集型：256 个任务，每任务 200,000 次递推')
$report.Add('')
$report.Add('| 执行方式 | 整批 ms | 相对串行加速比 | 相对单 worker 加速比 |')
$report.Add('| --- | --- | --- | --- |')
$single = Lookup cpu pool 1 total_ms
foreach ($workers in @(0, 1, 2, 4, 8)) {
    $mode = if ($workers -eq 0) { 'serial' } else { 'pool' }
    $label = if ($workers -eq 0) { '串行' } else { "$workers worker" }
    $time = Lookup cpu $mode $workers total_ms
    $report.Add("| $label | $(Cell $time) | $(Number $time.speedup_vs_serial)x | $(Number ($single.median / $time.median))x |")
}
$report.Add('')
$report.Add('## 关闭延迟：ms')
$report.Add('')
$report.Add('| 方式 | worker | 空闲 | 半数繁忙 | 全部繁忙 |')
$report.Add('| --- | --- | --- | --- | --- |')
foreach ($mode in @('shutdown', 'shutdown_now')) {
    foreach ($workers in @(1, 2, 4, 8)) {
        $report.Add("| $mode | $workers | $(Cell (Lookup close_idle $mode $workers close_ms)) | $(Cell (Lookup close_partial $mode $workers close_ms)) | $(Cell (Lookup close_all $mode $workers close_ms)) |")
    }
}
$report.Add('')
$report.Add('关闭队列没有未领取任务。繁忙 worker 计时前已领取并阻塞，开始关闭时共同放行，每个执行 2,000,000 次递推；计时包含一次放行信号和剩余计算等待及 worker join，不是单纯接口开销。1 worker 无半数繁忙配置。此处 shutdown_now 没有可丢弃积压，与 shutdown 都须等待已领取任务。')
$report.Add('')
$report.Add('## 计时与复现说明')
$report.Add('')
$report.Add('steady_clock 墙钟；单生产者；每轮打乱 worker/关闭方式顺序（固定随机种子）；预热不进入统计，不删离群值。建池、worker 启动、future 容器 reserve、关闭和 CSV 写入不计入任务批次。整批计时包含 submit、任务执行重叠、等待以及 future.get 和 checksum 累加；收集阶段不等于纯 future.get 调用成本。独立编译的 noinline 计算函数、禁用基准 IPO、运行期结果校验防止计算被消除。')
$report.Add('')
$report.Add("电源方案：$($metadata.power_scheme)。未固定频率、亲和性或调整进程优先级，未关闭其他应用。仅代表本机此次合成负载；没有用 Debug 数据或去掉异常轮次。完整方法与命令见 [PERFORMANCE.md](../../../docs/PERFORMANCE.md)。")
$report | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'report.md') -Encoding utf8
$metadata | Add-Member -Force -NotePropertyName analysis_script_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
$metadata | Add-Member -Force -NotePropertyName analysis_completed_at -NotePropertyValue ([DateTimeOffset]::Now.ToOffset([TimeSpan]::FromHours(8)).ToString('o'))
foreach ($artifact in @('samples.csv', 'summary.csv', 'report.md')) {
    $property = $artifact.Replace('.', '_') + '_sha256'
    $metadata | Add-Member -Force -NotePropertyName $property -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $ResultsDirectory $artifact) -Algorithm SHA256).Hash
}
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ResultsDirectory 'environment.json') -Encoding utf8
