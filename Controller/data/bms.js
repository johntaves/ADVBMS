$(function () {
    $("#email").click(function(event) {
        $.ajax({
            type: "POST",
            url: "/email",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        event.preventDefault();
    });
    $("#resetBaud").click(function(event) {
        $.ajax({
            type: "POST",
            url: "/resetBaud",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        event.preventDefault();
    });
    $("#menu > a").on('click',function (event) {
        $('#menu > a').removeClass("active");
        $(this).addClass("active");
        showContent($(this).attr("href"));
        getSettings();
        event.preventDefault();
    });
    $(".error,.success").hide();
    $("#maxY,#minY").change(function () {
        config.options.scales.yAxes[0].ticks.max = parseFloat($("#maxY").val());
        config.options.scales.yAxes[0].ticks.min = parseFloat($("#minY").val());
        myChart.update();
    });
    $("#soc a").click(function(event) {
        $.ajax({
            type: "POST",
            url: "/fullChg",
            dataType:'json',
            success: function (data) {
                if (data.val == "off")
                    $("#soc .v").addClass("manoff");
                else $("#soc .v").removeClass("manoff");
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
    $("#maxdiffvolts a").click(function(event) {
        $.ajax({
            type: "GET",
            url: "/clrMaxDiff",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
    $("#lastEventMsg a").click(function(event) {
        $.ajax({
            type: "GET",
            url: "/hideLastEventMsg",
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            }
        });
        return false;
    });
    showContent("cells");
    initChart();
    Setup();
    queryBMS();
});

var RELAY_TOTAL=0;
var nCells=0,nBanks=0;
var myChart,config;
var nonCellLines = 4;

function showCellCali(c) {
    $("#CellADCPlace > div").hide();
    $("#"+c).show();
}
function showContent(c) {
    $("#content > div").hide();
    $("#"+c).show();
}

function initChart() {
    config = {
        type: 'line',
        data: {
            labels: [],
            datasets: []
        },
        options: {
            responsive: true,
            legend: {
                display: false
            },
            tooltips: {
                mode: 'index',
                intersect: false,
            },
            hover: {
                mode: 'nearest',
                intersect: true
            },
            scales: {
                xAxes: [{
                    type: 'time',
                    time: {
                        parser: 'HH:mm:ss',
                        unit: 'second'
                    },
                    scaleLabel: {
                        display: true,
                        labelString: 'Date'
                    }
                }],
                yAxes: [{
                    display: true,
                    id: 'cv',
                    scaleLabel: {
                        display: true,
                        labelString: 'Cell Volts'
                    },
                    ticks: { max: 3.6, min: 3.1 }
                },{
                    display: true,
                    id: 'pv',
                    scaleLabel: {
                        display: true,
                        labelString: 'Volts'
                    },
                    ticks: { max: 28, min: 22 }
                },{
                    display: true,
                    id: 'pa',
                    scaleLabel: {
                        display: true,
                        labelString: 'Amps'
                    },
                    ticks: { max: 60, min: -100 }
                },{
                    display: true,
                    id: 'sa',
                    scaleLabel: {
                        display: true,
                        labelString: 'PV Amps'
                    },
                    ticks: { max: 60, min: 0 }
                },{
                    display: true,
                    id: 'f',
                    scaleLabel: {
                        display: true,
                        labelString: 'FSR'
                    },
                    ticks: { max: 2000, min: 1000 }
                }]
            }
        }
    };
    myChart = new Chart(document.getElementById('cellCan'), config);
}

var colors = [ '#e6194B', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#42d4f4', '#f032e6', '#fabed4', '#469990', '#dcbeff', '#9A6324', '#fffac8', '#800000', '#aaffc3', '#000075', '#a9a9a9' ];

function initCells() {
    config.data.labels = [];
    config.data.datasets = [];
    config.data.datasets[0] = {
        label: 'Pack V',
        backgroundColor: '#ffffff',
        borderColor: '#ffffff',
        yAxisID: 'pv',
        data: [ ],
        fill: false
    };
    config.data.datasets[1] = {
        label: 'Pack A',
        backgroundColor: '#000000',
        borderColor: '#000000',
        yAxisID: 'pa',
        data: [ ],
        fill: false
    };
    config.data.datasets[2] = {
        label: 'Solar A',
        backgroundColor: '#CCCCCC',
        borderColor: '#CCCCCC',
        yAxisID: 'sa',
        data: [ ],
        fill: false
    };
    config.data.datasets[3] = {
        label: 'FSR',
        backgroundColor: '#555555',
        borderColor: '#555555',
        yAxisID: 'f',
        data: [ ],
        fill: false
    };
    for (var i=0;i<nCells;i++) {
        config.data.datasets[i+nonCellLines] = {
            label: 'Cell '+i,
            backgroundColor: colors[i],
            borderColor: colors[i],
            yAxisID: 'cv',
            data: [ ],
            fill: false
        }
    }
    myChart.update();
    $("[cellval]").remove();
    for (var rel=0;rel<nCells;rel++) {
        var temp = $("#cellTV").clone();
        temp.attr({id: "cellTV"+rel});
        temp.find(".h").text('#'+rel+': T mV').css("background-color", colors[rel]);
        temp.attr({cellval: true});
        temp.insertBefore("#cellTV");
        var theA = temp.find("a");
        theA.attr("cell",rel);
        theA.click(function () {
            var r = $(this).attr("cell");
            $.ajax({
                type: "POST",
                url: "/dump",
                data: { cell: r},
                dataType:'json',
                success: function (data) {
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.show();
    }
    for (var tn=0;tn<nCells;tn++) {
        var temp = $("#CellADC").clone();
        temp.attr({cellval: true});
        temp.attr({id: "CellADC"+tn});
        $.each(['cellCbCoef','cellCaddr','cellCmul','cellCdiv','cellCrange','cellCcur','cellCRawcur','celladdr','cellmul','celldiv','cellrange'
            ,'cellVRawcur','cellVcur','sampb','sampa','applyButt','cellFails'],function (index,value) {
            temp.find('#'+value).attr({id: value+tn, name: value+tn});
        });
        $.each(['samp','sampadc','sampt'],function (index,value) {
            for (var i=0;i<4;i++) {
                var theid = value + i + '_';
                temp.find('#'+theid).attr({id: theid+tn, name: theid+tn});
            }
        });
        temp.find("#applyButt"+tn).attr({cell: tn}).click(function (e) {
            e.preventDefault();
            $.ajax({
                type: "POST",
                url: "/apply",
                data: { cell: $(this).attr('cell')},
                dataType:'json',
                success: function (data) {
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });

        });
        $("#CellADCPlace").append(temp);
        temp.show();

        temp = $("#CellMenuTemp").clone();
        temp.attr({cellval: true});
        temp.text("Cell "+tn);
        if (tn == 0) temp.addClass("active");
        temp.attr({href: "CellADC"+tn});
        $("#CellsMenu").append(temp);
        temp.show();
    }
    $("#cellsMenu > a").on('click',function (event) {
        $('#cellsMenu > a').removeClass("active");
        $(this).addClass("active");
        showCellCali($(this).attr("href"));
        event.preventDefault();
    });
    showCellCali("CellADC0");
    $("form").unbind('submit').submit(doSubmit);
}

function getSettings() {
    $.getJSON("settings",
        function (data) {
            $("input[name='apName']").val(data.apName);
            $("input[name='apPW']").val("");
            $("input[name='password']").val("");
            $("input[name='email']").val(data.email);
            $("input[name='senderEmail']").val(data.senderEmail);
            $("input[name='senderServer']").val(data.senderServer);
            $("input[name='senderPort']").val(data.senderPort);
            $("input[name='senderSubject']").val(data.senderSubject);
            $("input[name='logEmail']").val(data.logEmail);
            $("input[name='doLogging']").prop("checked", data.doLogging);
            $("#cellBaud").val(data.cellBaud);

            $("#Avg").val(data.Avg);
            $("#ConvTime").val(data.ConvTime);
            $("#BattAH").val(data.BattAH);
            $("#TopAmps").val(data.TopAmps);
            $("#socLastAdj").html(data.socLastAdj);
            $("#socAvgAdj").html(data.socAvgAdj);
            $("#BatAHMeasured").html(data.BatAHMeasured);
            $("#MaxAmps").val(data.MaxAmps);
            $("#PVMaxAmps").val(data.PVMaxAmps);
            $("#ShuntUOhms").val(data.ShuntUOhms);
            $("#PVShuntUOhms").val(data.PVShuntUOhms);
            $("#nBanks").val(data.nBanks);
            $("#nCells").val(data.nCells);
            $("#ChargePct").val(data.ChargePct);
            $("#ChargePctRec").val(data.ChargePctRec);
            $("#FloatV").val(data.FloatV);
            $("#ChargeRate").val(data.ChargeRate);
            $("#CellsOutMin").val(data.CellsOutMin);
            $("#CellsOutMax").val(data.CellsOutMax);
            $("#CellsOutTime").val(data.CellsOutTime);
            
            $.each(data.tempSettings,function (index,value) {
                $("#bCoef"+index).val(value.bCoef);
                $("#addr"+index).val(value.addr);
                $("#mul"+index).val(value.mul);
                $("#div"+index).val(value.div);
                $("#range"+index).val(value.range);
            });

            $.each(data.cellSettings,function (index,value) {
                $("#cellCbCoef"+index).val(value.bCoef);
                $("#cellCaddr"+index).val(value.cellCaddr);
                $("#cellCmul"+index).val(value.cellCmul);
                $("#cellCdiv"+index).val(value.cellCdiv);
                $("#cellCrange"+index).val(value.cellCrange);
                $("#celladdr"+index).val(value.celladdr);
                $("#cellmul"+index).val(value.cellmul);
                $("#celldiv"+index).val(value.celldiv);
                $("#cellrange"+index).val(value.cellrange);
                for (var i=0;i<4;i++) {
                    if (value["samp"+i]) {
                        $("#samp"+i+"_"+index).val(value["samp"+i]);
                        $("#sampadc"+i+"_"+index).text(value["sampadc"+i]);
                        $("#sampt"+i+"_"+index).text(value["sampt"+i]);
                    } else {
                        $("#samp"+i+"_"+index).val('');
                        $("#sampadc"+i+"_"+index).text('');
                        $("#sampt"+i+"_"+index).text('');
                    }
               }
                var b = value.sampb;
                $("#sampb"+index).text(b / 10000.0);
                $("#sampa"+index).text(value.sampa);
            });

            $("#CurSOC").val('');
            $("#PollFreq").val(data.PollFreq);
            $("input[name='ssid'").val(data.ssid);

            $.each(data.limitSettings, function (index, value) {
                $("#" + index).val(value);
            });
            $("#useTemp1").prop("checked", data.useTemp1);
            $("#useTemp2").prop("checked", data.useTemp2);
            $("#useCellC").prop("checked", data.useCellC);

            $.each(data.relaySettings, function (index, value) {
                $("#relayName" + index).val(value.name);
                $("#relayFrom" + index).val(value.from);
                setRelayType.call($("#relayType" + index).val(value.type));
                $("#relayTrip" + index).val(value.trip);
                $("#relayRec" + index).val(value.rec);
            });
    
        }).fail(function () { }
        );
    return true;
}

function formatNum(n,d) {
    return n.toLocaleString(undefined, { maximumFractionDigits: d, minimumFractionDigits: d });
}

function queryBMS() {
    var pollFreq = 2000;
    var histSize = 100;
    $.getJSON("status", function (data) {
        if (data.debugstr) $("#debugstr").show().html(data.debugstr);
        else $("#debugstr").hide();lastEventMsg
        if (data.lastEventMsg) {
            $("#lastEventMsg").show();
            $("#lastEventMsg a").html(data.lastEventMsg);
        } else $("#lastEventMsg").hide();

        if (data.RELAY_TOTAL && !RELAY_TOTAL) {
            setupRelays(data.RELAY_TOTAL);
            RELAY_TOTAL = data.RELAY_TOTAL;
        }
        for (var i=0;i<RELAY_TOTAL;i++) {
            var rn = data["relayName"+i];
            if (rn && rn.length) {
                $("#relayStatus"+i).show();
                $("#relayStatus"+i+" .h").html(rn);
                $("#relayStatus"+i+" .v").html(data["relayStatus"+i]);
                if (data["relayOff"+i] == "off")
                    $("#relayStatus"+i+" .v").addClass("manoff");
                else $("#relayStatus"+i+" .v").removeClass("manoff");
            } else $("#relayStatus"+i).hide();
        }
        if (data.watchDogHits) {
            $("#watchDogHits").show();
            $("#watchDogHits .v").html(data.watchDogHits);
        } else $("#watchDogHits").hide();

        var d = new Date(1000 * data.now);
        $("#timenow").html(d.toLocaleTimeString());
        $("#datenow").html(d.toLocaleDateString());
        $("#version").html(data.version);

        $("#cellvolt").hide();
        if (data.maxCellVState || data.minCellVState)
            $("#cellvolt").show();
        val = "";
        if (data.maxCellVState)
            val = "over";
        if (data.minCellVState)
            val = val + " under";
        $("#cellvolt .v").html(val);

        var packvolts = Number(data.packvolts)/1000;
        var dval = formatNum(packvolts,2);
        if (data.maxPackVState || data.minPackVState)
            $("#packvolts .v").addClass("highlighted");
        else $("#packvolts .v").removeClass("highlighted");
        if (data.maxPackVState)
            dval = "over " + formatNum(packvolts,2);
        if (data.minPackVState)
            dval = "under " + formatNum(packvolts,2);
        $("#packvolts .v").html(dval);
        $("#maxdiffvolts .v").html(formatNum(Number(data.maxdiffvolts)/1000,2));

        var pc = Number(data.packcurrent);
        var pvc = Number(data.pvcurrent);
        var lc = (pvc - pc)/1000;
        pc = pc / 1000;
        pvc = pvc / 1000;
        $("#loadcurrent .v").html(formatNum(lc,2));
        $("#packcurrent .v").html(formatNum(pc,2));
        $("#pvcurrent .v").html(formatNum(pvc,2));
        $("#soc .v").html(data.soc);
        $("#soc .v").removeClass("manoff");
        $("#soc .v").removeClass("fullChg");
        if (!data.socvalid)
            $("#soc .v").addClass("manoff");
        else if (data.fullChg)
            $("#soc .v").addClass("fullChg");

        $("#celltemp").hide();
        if (data.maxCellCState || data.minCellCState)
            $("#celltemp").show();
        var val = "";
        if (data.maxCellCState)
            val = "over";
        if (data.minCellCState)
            val = val + " under";
        $("#celltemp .v").html(val);

        val = data.temp1;
        if (data.maxPackCState || data.minPackCState)
            $("#temp1 .v").addClass("highlighted")
        else $("#temp1 .v").removeClass("highlighted")
        if (data.maxPackCState)
            val = "over " + val;
        if (data.minPackCState)
            val = "under " + val;
        $("#temp1 .v").html(val);
        val = data.temp2;
        if (val < -100)
            $("#temp2").hide();
        else {
            if (data.maxPackCState)
                val = "over " + val;
            if (data.minPackCState)
                val = "under " + val;
            $("#temp2").show();
            $("#temp2 .v").html(val);
        }
        $('#fsr .v').html(data.fsr);
        $('#cur0').text(data.rawTemp1);
        $('#cur1').text(data.rawTemp2);
        $('#cur2').text(data.fsr);

        $("#uptime .v").html(data.uptime);

        if (data.nocells == true) {
            $("#nocells").show();
        } else {
            $("#nocells").fadeOut();
        }
        if (data.pkts) $("#pkts .v").html(data.pkts);

        $("#nocontroller").hide();

        if (data.nCells != nCells) {
            nCells = data.nCells;
            nBanks = data.nBanks;
            initCells();
        }
        var dt = new Date();
        config.data.labels.push(dt);
        var t = new Date(dt.getTime() - (pollFreq*histSize));
        while (config.data.labels.length && config.data.labels[0] < t) {
            config.data.labels.shift();
            for (var i=0;i<nonCellLines;i++)
                config.data.datasets[i].data.shift();
            $.each(data.cells, function (index, value) {
                config.data.datasets[value.c+nonCellLines].data.shift();
            });
        }
        config.data.datasets[0].data.push(packvolts);
        config.data.datasets[1].data.push(pc);
        config.data.datasets[2].data.push(pvc);
        config.data.datasets[3].data.push(data.fsr);
        $.each(data.cells, function (index, value) { // does not handle multiple banks, multiple charts are needed
            var ds = config.data.datasets[value.c+nonCellLines];
            if (data.nocells) ds.pointStyle = 'cross';
            else ds.pointStyle = 'circle';
            var cData = ds.data;
            var valV = value.v / 1000.0;
            cData.push(valV);
            $("#cellVRawcur"+index).text(value.rv);
            $("#cellCRawcur"+index).text(value.rt);
            $("#cellVcur"+index).text(value.v);
            $("#cellCcur"+index).text(value.t);
            $("#cellFails"+index).text(value.f);
            var cell = $("#cellTV"+value.c+" .v");
            if (value.v > 0) {
                if (value.t > -101)
                    $("#cellTV"+value.c+" .t").html(value.t);
                else $("#cellTV"+value.c+" .t").html("");
                cell.text(formatNum(valV,3));
            } else cell.text('');
            if (value.d) cell.addClass("dumping");
            else cell.removeClass("dumping");
        });
        myChart.update();
        if (data.nocells) $("#cellsRow .v,.t").addClass("manoff");
        else $("#cellsRow .v,.t").removeClass("manoff");
        setTimeout(queryBMS, pollFreq);
    }).fail(function () {
        $("#nocontroller").show();
        setTimeout(queryBMS, pollFreq);
    });
}

function convertTemp(doC,t) {
    var t = $(t + " .v").html();
    if (doC) t = (t-32)*5/9;
    else t = t*9/5 + 32;
    $(t + " .v").html(Math.round(t));
}

function toggleTemp() {
    $.ajax({
        type: "GET",
        url: "/toggleTemp",
        success: function (data) {
            convertTemp(data.val,"#temp1");
            convertTemp(data.val,"#temp2");
            $("#savesuccess").show().delay(2000).fadeOut(500);
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        }
    });
    return false;
}

function setRelayType() {
    var r = $(this).attr("relay");
    var val = $(this).val();
    if (val == "CP" || val == "LP")
        $("#relayDoSoC"+r).show();
    else $("#relayDoSoC"+r).hide();
    if (val == "L" || val == "LP")
    $("#relayDoFrom"+r).show();
    else $("#relayDoFrom"+r).hide();
}

function setupRelays(rt) {
    for (var rel=0;rel<rt;rel++) {
        var temp = $("#relay").clone();
        temp.attr({id: "relay"+rel});
        temp.find("[for='relayName']").text("J"+rel+":");
        $.each(['Name','DoSoC','Type','Trip','Rec','DoFrom','From'],function (index,value) {
            temp.find('#relay'+value).attr({id: "relay"+value+rel, name: "relay"+value+rel});
        });
        temp.find("[for]").each(function(index) {
            var val=$(this).attr("for");
            $(this).attr({for: val+rel});
        });
        var theTy = temp.find("#relayType" + rel);
        theTy.attr("relay",rel);
        theTy.change(setRelayType);
        temp.insertBefore("#relay");
        temp = $("#relayStatus").clone();
        temp.attr({id: "relayStatus"+rel});
        var theA = temp.find("a");
        theA.attr("relay",rel);
        theA.click(function () {
            var r = $(this).attr("relay");
            $.ajax({
                type: "POST",
                url: "/saveOff",
                data: { relay: r},
                dataType:'json',
                success: function (data) {
                    var r = data.relay;
                    if (data.val == "off")
                        $("#relayStatus"+r+" .v").addClass("manoff");
                    else $("#relayStatus"+r+" .v").removeClass("manoff");
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.insertBefore("#relayStatus");
    }
    $("#relay").remove();
    $("#relayStatus").remove();
}

function doSubmit(e) {
    e.preventDefault();
    $("#errmess").hide();
    $.ajax({
        type: $(this).attr('method'),
        url: $(this).attr('action'),
        data: $(this).serialize(),
        success: function (data) {
            $("#savesuccess").show().delay(2000).fadeOut(500);
            if (!data.success) {
                $("#errmess").show().text(data.errmess);
            }
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        },
    });
}

function Setup() {

    $("#loading").show();

    $("#temp1 a").click(toggleTemp);
    $("#temp2 a").click(toggleTemp);
    var LIMITNames = [['Volts (mV):','Temp (C):'],['Cell','Pack'],['Max','Min'],['Trip:','Rec:']];
    for (var l0=0;l0<LIMITNames[0].length;l0++) {
        for (var l1=0;l1<LIMITNames[1].length;l1++) {
            for (var l2=0;l2<LIMITNames[2].length;l2++) {
                var d = $("<div class='limits'></div>");
                for (var l3=0;l3<LIMITNames[3].length;l3++) {
                    var id=l0+""+l1+""+l2+""+l3;
                    var temp = $("#limit").clone();
                    var lab = temp.find("label");
                    if (l3) {
                        lab.html(LIMITNames[3][l3]);
                        lab.addClass('rec');
                    } else {
                        lab.addClass('field');
                        lab.html(LIMITNames[2][l2] + ' ' + LIMITNames[1][l1] + ' ' + LIMITNames[0][l0]);
                    }
                    var inp=temp.find("input");
                    inp.attr("for",id);
                    inp.attr("id",id);
                    inp.attr("name",id);
                    d.append(temp.children());
                }
                d.insertBefore("#limit");
            }
        }
    }
    $("#limit").remove();
    
    $("form").unbind('submit').submit(doSubmit);
        
}